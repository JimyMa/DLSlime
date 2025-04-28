#pragma once

#include <atomic>
#include <map>
#include <vector>

#include <zmq.hpp>

#include "engine/assignment.h"
#include "engine/rdma/rdma_assignment.h"
#include "engine/rdma/rdma_transport.h"

namespace slime {

/**
 * To aggregate send among different NIC devices,
 * we have to slice user register data_ptr to different MR
 * bound to different device rdma context
 */
struct DevMrSlice {
    int         rdma_ctx_index;
    std::string mr_key;
    uintptr_t   origin_data_ptr;
    uintptr_t   offset_data_ptr;
    size_t      length;

    DevMrSlice(int                rdma_ctx_index,
               const std::string& mr_key,
               uintptr_t          origin_data_ptr,
               uintptr_t          offset_data_ptr,
               size_t             length):
        rdma_ctx_index(rdma_ctx_index),
        mr_key(mr_key),
        origin_data_ptr(origin_data_ptr),
        offset_data_ptr(offset_data_ptr),
        length(length)
    {
    }

    DevMrSlice(const DevMrSlice& dev_mr_slice) = default;
    DevMrSlice(DevMrSlice&& dev_mr_slice)      = default;
};

class RDMAScheduler {
public:
    RDMAScheduler();
    ~RDMAScheduler();

    int64_t register_memory_region(const std::string& mr_key, uintptr_t data_ptr, size_t length);

    int connectRemoteNode(const std::string& remote_addr, int remote_port, int local_port);

    RDMASchedulerAssignment submitAssignment(AssignmentBatch& assignment);

    int teriminate();

    int waitRemoteTeriminate();

private:
    int selectRdma();

    json rdma_exchange_info();

    void resetTcpSockets();

    const static int64_t SPLIT_ASSIGNMENT_BYTES      = (8ull << 20);
    const static int64_t SPLIT_ASSIGNMENT_BATCH_SIZE = 8192;
    const static int     PORT_EACH_DEVICE            = 1;

    std::vector<RDMAContext>                               rdma_ctxs_;
    std::map<std::string, std::map<uintptr_t, DevMrSlice>> virtual_mr_to_actual_mr_;
    std::atomic<int>                                       split_assignment_done_cnt_;
    std::map<int, AssignmentBatch>                         rdma_index_to_assignments_;
    int                                                    assignment_cnt_      = 0;
    int                                                    last_rdma_selection_ = -1;

    zmq::context_t* tcp_context_ = nullptr;
    zmq::socket_t*  send_        = nullptr;
    zmq::socket_t*  recv_        = nullptr;
};

};  // namespace slime
