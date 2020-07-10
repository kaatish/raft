/*
 * Copyright (c) 2020, NVIDIA CORPORATION.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <nccl.h>

#include <raft/comms/comms.hpp>

#include <ucp/api/ucp.h>
#include <ucp/api/ucp_def.h>
#include "ucp_helper.hpp"

#include <nccl.h>

#include <stdlib.h>
#include <time.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <raft/handle.hpp>

#include <thread>

#include <cuda_runtime.h>

#include <raft/cudart_utils.h>
#include <raft/error.hpp>

namespace raft {

/**
 * @brief Exception thrown when a NCCL error is encountered.
 */
struct nccl_error : public raft::exception {
  explicit nccl_error(char const *const message) : raft::exception(message) {}
  explicit nccl_error(std::string const &message) : raft::exception(message) {}
};

}  // namespace raft

/**
 * @brief Error checking macro for NCCL runtime API functions.
 *
 * Invokes a NCCL runtime API function call, if the call does not return ncclSuccess, throws an
 * exception detailing the NCCL error that occurred
 */
#define NCCL_TRY(call)                                                        \
  do {                                                                        \
    ncclResult_t const status = (call);                                       \
    if (ncclSuccess != status) {                                              \
      std::string msg{};                                                      \
      SET_ERROR_MSG(msg,                                                      \
                    "NCCL error encountered at: ", "call='%s', Reason=%d:%s", \
                    #call, status, ncclGetErrorString(status));               \
      throw raft::nccl_error(msg);                                            \
    }                                                                         \
  } while (0);

#define NCCL_CHECK_NO_THROW(call)                         \
  do {                                                    \
    ncclResult_t status = call;                           \
    if (ncclSuccess != status) {                          \
      printf("NCCL call='%s' failed. Reason:%s\n", #call, \
             ncclGetErrorString(status));                 \
    }                                                     \
  } while (0)

namespace raft {
namespace comms {

static size_t get_datatype_size(const datatype_t datatype) {
  switch (datatype) {
    case datatype_t::CHAR:
      return sizeof(char);
    case datatype_t::UINT8:
      return sizeof(uint8_t);
    case datatype_t::INT32:
      return sizeof(int);
    case datatype_t::UINT32:
      return sizeof(unsigned int);
    case datatype_t::INT64:
      return sizeof(int64_t);
    case datatype_t::UINT64:
      return sizeof(uint64_t);
    case datatype_t::FLOAT32:
      return sizeof(float);
    case datatype_t::FLOAT64:
      return sizeof(double);
    default:
      RAFT_FAIL("Unsupported datatype.");
  }
}

static ncclDataType_t get_nccl_datatype(const datatype_t datatype) {
  switch (datatype) {
    case datatype_t::CHAR:
      return ncclChar;
    case datatype_t::UINT8:
      return ncclUint8;
    case datatype_t::INT32:
      return ncclInt;
    case datatype_t::UINT32:
      return ncclUint32;
    case datatype_t::INT64:
      return ncclInt64;
    case datatype_t::UINT64:
      return ncclUint64;
    case datatype_t::FLOAT32:
      return ncclFloat;
    case datatype_t::FLOAT64:
      return ncclDouble;
    default:
      throw "Unsupported";
  }
}

static ncclRedOp_t get_nccl_op(const op_t op) {
  switch (op) {
    case op_t::SUM:
      return ncclSum;
    case op_t::PROD:
      return ncclProd;
    case op_t::MIN:
      return ncclMin;
    case op_t::MAX:
      return ncclMax;
    default:
      throw "Unsupported";
  }
}

class std_comms : public comms_iface {
 public:
  std_comms() = delete;

  /**
   * @brief Constructor for collective + point-to-point operation.
   * @param comm initialized nccl comm
   * @param ucp_worker initialized ucp_worker instance
   * @param eps shared pointer to array of ucp endpoints
   * @param size size of the cluster
   * @param rank rank of the current worker
   */
  std_comms(ncclComm_t nccl_comm, ucp_worker_h ucp_worker,
            std::shared_ptr<ucp_ep_h *> eps, int num_ranks, int rank,
            const std::shared_ptr<mr::device::allocator> device_allocator,
            cudaStream_t stream)
    : nccl_comm_(nccl_comm),
      stream_(stream),
      num_ranks_(num_ranks),
      rank_(rank),
      ucp_worker_(ucp_worker),
      ucp_eps_(eps),
      next_request_id_(0),
      device_allocator_(device_allocator) {
    initialize();
  };

  /**
   * @brief constructor for collective-only operation
   * @param comm initilized nccl communicator
   * @param size size of the cluster
   * @param rank rank of the current worker
   */
  std_comms(const ncclComm_t nccl_comm, int num_ranks, int rank,
            const std::shared_ptr<mr::device::allocator> device_allocator,
            cudaStream_t stream)
    : nccl_comm_(nccl_comm),
      stream_(stream),
      num_ranks_(num_ranks),
      rank_(rank),
      device_allocator_(device_allocator) {
    initialize();
  };

  virtual ~std_comms() {
    device_allocator_->deallocate(sendbuff_, sizeof(int), stream_);
    device_allocator_->deallocate(recvbuff_, sizeof(int), stream_);
  }

  void initialize() {
    sendbuff_ = reinterpret_cast<int *>(
      device_allocator_->allocate(sizeof(int), stream_));
    recvbuff_ = reinterpret_cast<int *>(
      device_allocator_->allocate(sizeof(int), stream_));
  }

  int get_size() const { return num_ranks_; }

  int get_rank() const { return rank_; }

  std::unique_ptr<comms_iface> comm_split(int color, int key) const {
    // Not supported by NCCL
    ASSERT(false,
           "ERROR: commSplit called but not yet supported in this comms "
           "implementation.");
  }

  void barrier() const {
    CUDA_CHECK(cudaMemsetAsync(sendbuff_, 1, sizeof(int), stream_));
    CUDA_CHECK(cudaMemsetAsync(recvbuff_, 1, sizeof(int), stream_));

    allreduce(sendbuff_, recvbuff_, 1, datatype_t::INT32, op_t::SUM, stream_);

    ASSERT(sync_stream(stream_) == status_t::SUCCESS,
           "ERROR: syncStream failed. This can be caused by a failed rank_.");
  }

  void get_request_id(request_t *req) const {
    request_t req_id;

    if (this->free_requests_.empty())
      req_id = this->next_request_id_++;
    else {
      auto it = this->free_requests_.begin();
      req_id = *it;
      this->free_requests_.erase(it);
    }
    *req = req_id;
  }

  void isend(const void *buf, size_t size, int dest, int tag,
             request_t *request) const {
    ASSERT(ucp_worker_ != nullptr,
           "ERROR: UCX comms not initialized on communicator.");

    get_request_id(request);
    ucp_ep_h ep_ptr = (*ucp_eps_)[dest];

    ucp_request *ucp_req = (ucp_request *)malloc(sizeof(ucp_request));

    this->ucp_handler_.ucp_isend(ucp_req, ep_ptr, buf, size, tag,
                                 default_tag_mask, get_rank());

    requests_in_flight_.insert(std::make_pair(*request, ucp_req));
  }

  void irecv(void *buf, size_t size, int source, int tag,
             request_t *request) const {
    ASSERT(ucp_worker_ != nullptr,
           "ERROR: UCX comms not initialized on communicator.");

    get_request_id(request);

    ucp_ep_h ep_ptr = (*ucp_eps_)[source];

    ucp_tag_t tag_mask = default_tag_mask;

    ucp_request *ucp_req = (ucp_request *)malloc(sizeof(ucp_request));
    ucp_handler_.ucp_irecv(ucp_req, ucp_worker_, ep_ptr, buf, size, tag,
                           tag_mask, source);

    requests_in_flight_.insert(std::make_pair(*request, ucp_req));
  }

  void waitall(int count, request_t array_of_requests[]) const {
    ASSERT(ucp_worker_ != nullptr,
           "ERROR: UCX comms not initialized on communicator.");

    std::vector<ucp_request *> requests;
    requests.reserve(count);

    time_t start = time(NULL);

    for (int i = 0; i < count; ++i) {
      auto req_it = requests_in_flight_.find(array_of_requests[i]);
      ASSERT(requests_in_flight_.end() != req_it,
             "ERROR: waitall on invalid request: %d", array_of_requests[i]);
      requests.push_back(req_it->second);
      free_requests_.insert(req_it->first);
      requests_in_flight_.erase(req_it);
    }

    while (requests.size() > 0) {
      time_t now = time(NULL);

      // Timeout if we have not gotten progress or completed any requests
      // in 10 or more seconds.
      ASSERT(now - start < 10, "Timed out waiting for requests.");

      for (std::vector<ucp_request *>::iterator it = requests.begin();
           it != requests.end();) {
        bool restart = false;  // resets the timeout when any progress was made

        // Causes UCP to progress through the send/recv message queue
        while (ucp_handler_.ucp_progress(ucp_worker_) != 0) {
          restart = true;
        }

        auto req = *it;

        // If the message needs release, we know it will be sent/received
        // asynchronously, so we will need to track and verify its state
        if (req->needs_release) {
          ASSERT(UCS_PTR_IS_PTR(req->req),
                 "UCX Request Error. Request is not valid UCX pointer");
          ASSERT(!UCS_PTR_IS_ERR(req->req), "UCX Request Error: %d\n",
                 UCS_PTR_STATUS(req->req));
          ASSERT(req->req->completed == 1 || req->req->completed == 0,
                 "request->completed not a valid value: %d\n",
                 req->req->completed);
        }

        // If a message was sent synchronously (eg. completed before
        // `isend`/`irecv` completed) or an asynchronous message
        // is complete, we can go ahead and clean it up.
        if (!req->needs_release || req->req->completed == 1) {
          restart = true;

          // perform cleanup
          ucp_handler_.free_ucp_request(req);

          // remove from pending requests
          it = requests.erase(it);
        } else {
          ++it;
        }
        // if any progress was made, reset the timeout start time
        if (restart) {
          start = time(NULL);
        }
      }
    }
  }

  void allreduce(const void *sendbuff, void *recvbuff, size_t count,
                 datatype_t datatype, op_t op, cudaStream_t stream) const {
    NCCL_TRY(ncclAllReduce(sendbuff, recvbuff, count,
                           get_nccl_datatype(datatype), get_nccl_op(op),
                           nccl_comm_, stream));
  }

  void bcast(void *buff, size_t count, datatype_t datatype, int root,
             cudaStream_t stream) const {
    NCCL_TRY(ncclBroadcast(buff, buff, count, get_nccl_datatype(datatype), root,
                           nccl_comm_, stream));
  }

  void bcast(const void *sendbuff, void *recvbuff, size_t count,
             datatype_t datatype, int root, cudaStream_t stream) const {
    NCCL_TRY(ncclBroadcast(sendbuff, recvbuff, count,
                           get_nccl_datatype(datatype), root, nccl_comm_,
                           stream));
  }

  void reduce(const void *sendbuff, void *recvbuff, size_t count,
              datatype_t datatype, op_t op, int root,
              cudaStream_t stream) const {
    NCCL_TRY(ncclReduce(sendbuff, recvbuff, count, get_nccl_datatype(datatype),
                        get_nccl_op(op), root, nccl_comm_, stream));
  }

  void allgather(const void *sendbuff, void *recvbuff, size_t sendcount,
                 datatype_t datatype, cudaStream_t stream) const {
    NCCL_TRY(ncclAllGather(sendbuff, recvbuff, sendcount,
                           get_nccl_datatype(datatype), nccl_comm_, stream));
  }

  void allgatherv(const void *sendbuf, void *recvbuf, const size_t recvcounts[],
                  const int displs[], datatype_t datatype,
                  cudaStream_t stream) const {
    //From: "An Empirical Evaluation of Allgatherv on Multi-GPU Systems" - https://arxiv.org/pdf/1812.05964.pdf
    //Listing 1 on page 4.
    for (int root = 0; root < num_ranks_; ++root) {
      size_t dtype_size = get_datatype_size(datatype);
      NCCL_TRY(ncclBroadcast(
        sendbuf, static_cast<char *>(recvbuf) + displs[root] * dtype_size,
        recvcounts[root], get_nccl_datatype(datatype), root, nccl_comm_,
        stream));
    }
  }

  void reducescatter(const void *sendbuff, void *recvbuff, size_t recvcount,
                     datatype_t datatype, op_t op, cudaStream_t stream) const {
    NCCL_TRY(ncclReduceScatter(sendbuff, recvbuff, recvcount,
                               get_nccl_datatype(datatype), get_nccl_op(op),
                               nccl_comm_, stream));
  }

  status_t sync_stream(cudaStream_t stream) const {
    cudaError_t cudaErr;
    ncclResult_t ncclErr, ncclAsyncErr;
    while (1) {
      cudaErr = cudaStreamQuery(stream);
      if (cudaErr == cudaSuccess) return status_t::SUCCESS;

      if (cudaErr != cudaErrorNotReady) {
        // An error occurred querying the status of the stream_
        return status_t::ERROR;
      }

      ncclErr = ncclCommGetAsyncError(nccl_comm_, &ncclAsyncErr);
      if (ncclErr != ncclSuccess) {
        // An error occurred retrieving the asynchronous error
        return status_t::ERROR;
      }

      if (ncclAsyncErr != ncclSuccess) {
        // An asynchronous error happened. Stop the operation and destroy
        // the communicator
        ncclErr = ncclCommAbort(nccl_comm_);
        if (ncclErr != ncclSuccess)
          // Caller may abort with an exception or try to re-create a new communicator.
          return status_t::ABORT;
      }

      // Let other threads (including NCCL threads) use the CPU.
      std::this_thread::yield();
    }
  }

 private:
  ncclComm_t nccl_comm_;
  cudaStream_t stream_;

  int *sendbuff_, *recvbuff_;

  int num_ranks_;
  int rank_;

  comms_ucp_handler ucp_handler_;
  ucp_worker_h ucp_worker_;
  std::shared_ptr<ucp_ep_h *> ucp_eps_;
  mutable request_t next_request_id_;
  mutable std::unordered_map<request_t, struct ucp_request *>
    requests_in_flight_;
  mutable std::unordered_set<request_t> free_requests_;

  std::shared_ptr<mr::device::allocator> device_allocator_;
};
}  // end namespace comms
}  // end namespace raft
