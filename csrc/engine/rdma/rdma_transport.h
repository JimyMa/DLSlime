#pragma once

#include "engine/assignment.h"
#include "engine/rdma/memory_pool.h"
#include "engine/rdma/rdma_assignment.h"
#include "engine/rdma/rdma_config.h"

#include "utils/json.hpp"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <infiniband/verbs.h>

namespace slime {

using json = nlohmann::json;

class RDMAContext {
public:
    /*
      A link of rdma QP.
    */
    RDMAContext() {}

    ~RDMAContext()
    {
        stop_future();
    }

    /* Initialize */
    int64_t init(std::string dev_name, uint8_t ib_port, std::string link_type);

    /* RDMA Link Construction */
    int64_t connect_to(RDMAInfo remote_rdma_info);

    /* Memory Allocation */
    int64_t register_memory_region(std::string mr_key, uintptr_t data_ptr, size_t length)
    {
        memory_pool_.register_memory_region(mr_key, data_ptr, length);
        return 0;
    }

    int64_t register_remote_memory_region(std::string mr_key, json mr_info)
    {
        memory_pool_.register_remote_memory_region(mr_key, mr_info);
        return 0;
    }

    /* Async RDMA SendRecv */
    int64_t send_async(RDMAAssignment* assign);
    int64_t recv_async(RDMAAssignment* assign);

    /* Async RDMA Read */
    int64_t read_batch_async(RDMAAssignment* assign);

    /* Submit an assignment */
    int submit(RDMAAssignment* assignment);

    void launch_future();
    void stop_future();

    rdma_info_t get_local_rdma_info() const
    {
        return local_rdma_info_;
    }

    rdma_info_t get_remote_rdma_info() const
    {
        return remote_rdma_info_;
    }

    json local_info()
    {
        return json{{"rdma_info", local_rdma_info_.to_json()}, {"mr_info", memory_pool_.mr_info()}};
    }

    std::string get_dev_ib() const
    {
        return "@" + device_name_ + "#" + std::to_string(ib_port_);
    }

private:
    std::string device_name_ = "";

    /* RDMA Configuration */
    struct ibv_context*      ib_ctx_       = nullptr;
    struct ibv_pd*           pd_           = nullptr;
    struct ibv_comp_channel* comp_channel_ = nullptr;
    struct ibv_cq*           cq_           = nullptr;
    struct ibv_qp*           qp_           = nullptr;
    uint8_t                  ib_port_      = -1;

    MemoryPool memory_pool_;

    /* RDMA Exchange Information */
    rdma_info_t remote_rdma_info_;
    rdma_info_t local_rdma_info_;

    /* State Management */
    bool initialized_ = false;
    bool connected_   = false;

    /* Send Mutex */
    std::mutex rdma_post_send_mutex_;

    /* Assignment Queue */
    std::mutex                  assign_queue_mutex_;
    std::queue<RDMAAssignment*> assign_queue_;
    std::atomic<int>            outstanding_rdma_reads_{0};

    /* Has Runnable Assignment */
    std::condition_variable has_runnable_event_;

    /* async cq and wq handler */
    std::future<void> cq_future_;
    std::future<void> wq_future_;
    std::atomic<bool> stop_cq_future_{false};
    std::atomic<bool> stop_wq_future_{false};

    /* Completion Queue Polling */
    int64_t cq_poll_handle();
    /* Working Queue Dispatch */
    int64_t wq_dispatch_handle();
};

}  // namespace slime
