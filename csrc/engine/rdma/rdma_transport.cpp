#include "engine/rdma/rdma_transport.h"
#include "engine/assignment.h"
#include "engine/rdma/memory_pool.h"
#include "engine/rdma/rdma_assignment.h"
#include "engine/rdma/rdma_config.h"

#include "utils/ibv_helper.h"
#include "utils/logging.h"
#include "utils/utils.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <infiniband/verbs.h>
#include <stdexcept>

namespace slime {

typedef struct callback_info {
    callback_info(OpCode opcode, size_t batchsize, callback_fn_t& callback):
        opcode_(opcode), batchsize_(batchsize), callback_(callback)
    {
    }
    OpCode        opcode_;
    size_t        batchsize_;
    callback_fn_t callback_;
} callback_info_t;

int64_t RDMAContext::init(std::string dev_name, uint8_t ib_port, std::string link_type)
{
    device_name_ = dev_name;
    uint16_t      lid;
    enum ibv_mtu  active_mtu;
    union ibv_gid gid;
    int64_t       gidx;
    uint32_t      psn;

    if (initialized_) {
        SLIME_LOG_ERROR("Already initialized.");
        return -1;
    }

    /* Get RDMA Device Info */
    struct ibv_device** dev_list;
    struct ibv_device*  ib_dev;
    int                 num_devices;
    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list) {
        SLIME_LOG_ERROR("Failed to get RDMA devices list");
        return -1;
    }

    if (!num_devices) {
        SLIME_LOG_ERROR("No RDMA devices found.")
        return -1;
    }

    for (int i = 0; i < num_devices; ++i) {
        char* dev_name_from_list = (char*)ibv_get_device_name(dev_list[i]);
        if (strcmp(dev_name_from_list, dev_name.c_str()) == 0) {
            SLIME_LOG_INFO("found device " << dev_name_from_list);
            ib_dev  = dev_list[i];
            ib_ctx_ = ibv_open_device(ib_dev);
            break;
        }
    }

    if (!ib_ctx_) {
        SLIME_LOG_WARN("Can't find or failed to open the specified device, try to open "
                       "the default device "
                       << (char*)ibv_get_device_name(dev_list[0]));
        ib_ctx_ = ibv_open_device(dev_list[0]);
        if (!ib_ctx_) {
            SLIME_LOG_ERROR("Failed to open the default device");
            return -1;
        }
    }

    struct ibv_device_attr device_attr;
    if (ibv_query_device(ib_ctx_, &device_attr) != 0)
        SLIME_LOG_ERROR("Failed to query device");
    SLIME_LOG_DEBUG("Max Memory Region:" << device_attr.max_mr);
    SLIME_LOG_DEBUG("Max Memory Region Size:" << device_attr.max_mr_size);
    SLIME_LOG_DEBUG("Max Memory QP WR:" << device_attr.max_qp_wr);

    struct ibv_port_attr port_attr;
    ib_port_ = ib_port;
    if (ibv_query_port(ib_ctx_, ib_port, &port_attr)) {
        SLIME_LOG_ERROR("Unable to query port {} attributes\n" << ib_port_);
        return -1;
    }
    if ((port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND && link_type == "RoCE")
        || (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET && link_type == "IB")) {
        SLIME_LOG_ERROR("port link layer and config link type don't match");
        return -1;
    }
    if (port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND) {
        gidx = -1;
    }
    else {
        gidx = ibv_find_sgid_type(ib_ctx_, ib_port_, IBV_GID_TYPE_ROCE_V2, AF_INET);
        if (gidx < 0) {
            SLIME_LOG_ERROR("Failed to find GID");
            return -1;
        }
    }

    lid        = port_attr.lid;
    active_mtu = port_attr.active_mtu;

    /* Alloc Protected Domain (PD) */
    pd_ = ibv_alloc_pd(ib_ctx_);
    if (!pd_) {
        SLIME_LOG_ERROR("Failed to allocate PD");
        return -1;
    }
    memory_pool_ = MemoryPool(pd_);

    /* Alloc Complete Queue (CQ) */
    SLIME_ASSERT(ib_ctx_, "init rdma context first");
    comp_channel_ = ibv_create_comp_channel(ib_ctx_);
    cq_           = ibv_create_cq(ib_ctx_, MAX_SEND_WR + MAX_RECV_WR, NULL, comp_channel_, 0);
    SLIME_ASSERT(cq_, "create CQ failed");

    /* Create Queue Pair (QP) */
    struct ibv_qp_init_attr qp_init_attr = {};
    qp_init_attr.send_cq                 = cq_;
    qp_init_attr.recv_cq                 = cq_;
    qp_init_attr.qp_type                 = IBV_QPT_RC;  // Reliable Connection
    qp_init_attr.cap.max_send_wr         = MAX_SEND_WR;
    qp_init_attr.cap.max_recv_wr         = MAX_RECV_WR;
    qp_init_attr.cap.max_send_sge        = 1;
    qp_init_attr.cap.max_recv_sge        = 1;
    qp_init_attr.sq_sig_all              = false;

    qp_ = ibv_create_qp(pd_, &qp_init_attr);
    if (!qp_) {
        SLIME_LOG_ERROR("Failed to create QP");
        return -1;
    }

    /* Modify QP to INIT state */
    struct ibv_qp_attr attr = {};
    attr.qp_state           = IBV_QPS_INIT;
    attr.port_num           = ib_port_;
    attr.pkey_index         = 0;
    attr.qp_access_flags =
        IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_ATOMIC;

    int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;

    int ret = ibv_modify_qp(qp_, &attr, flags);
    if (ret) {
        SLIME_LOG_ERROR("Failed to modify QP to INIT");
    }

    /* Set Packet Sequence Number (PSN) */
    srand48(time(NULL));
    psn = lrand48() & 0xffffff;

    /* Get GID */
    if (gidx != -1 && ibv_query_gid(ib_ctx_, 1, gidx, &gid)) {
        SLIME_LOG_ERROR("Failed to get GID");
    }

    /* Set Local RDMA Info */
    local_rdma_info_.gidx = gidx;
    local_rdma_info_.qpn  = qp_->qp_num;
    local_rdma_info_.psn  = psn;
    local_rdma_info_.gid  = gid;
    local_rdma_info_.lid  = lid;
    local_rdma_info_.mtu  = (uint32_t)active_mtu;

    initialized_ = true;

    return 0;
}

int64_t RDMAContext::connect_to(RDMAInfo remote_rdma_info)
{
    int                ret;
    struct ibv_qp_attr attr = {};
    int                flags;

    SLIME_ASSERT(!connected_, "Already connected!");
    remote_rdma_info_ = std::move(remote_rdma_info);

    // Modify QP to Ready to Receive (RTR) state
    memset(&attr, 0, sizeof(attr));
    attr.qp_state           = IBV_QPS_RTR;
    attr.path_mtu           = (enum ibv_mtu)std::min((uint32_t)remote_rdma_info_.mtu, (uint32_t)local_rdma_info_.mtu);
    attr.dest_qp_num        = remote_rdma_info_.qpn;
    attr.rq_psn             = remote_rdma_info_.psn;
    attr.max_dest_rd_atomic = 16;
    attr.min_rnr_timer      = 12;
    attr.ah_attr.dlid       = 0;
    attr.ah_attr.sl         = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num      = 1;

    if (local_rdma_info_.gidx == -1) {
        // IB
        attr.ah_attr.dlid      = local_rdma_info_.lid;
        attr.ah_attr.is_global = 0;
    }
    else {
        // RoCE v2
        attr.ah_attr.is_global      = 1;
        attr.ah_attr.grh.dgid       = remote_rdma_info.gid;
        attr.ah_attr.grh.sgid_index = local_rdma_info_.gidx;
        attr.ah_attr.grh.hop_limit  = 1;
    }

    flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC
            | IBV_QP_MIN_RNR_TIMER;

    ret = ibv_modify_qp(qp_, &attr, flags);
    if (ret) {
        SLIME_LOG_ERROR("Failed to modify QP to RTR: reason: " << strerror(ret));
        return -1;
    }

    // Modify QP to RTS state
    memset(&attr, 0, sizeof(attr));
    attr.qp_state      = IBV_QPS_RTS;
    attr.timeout       = 14;
    attr.retry_cnt     = 7;
    attr.rnr_retry     = 7;
    attr.sq_psn        = local_rdma_info_.psn;
    attr.max_rd_atomic = 16;

    flags =
        IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;

    ret = ibv_modify_qp(qp_, &attr, flags);
    if (ret) {
        SLIME_LOG_ERROR("Failed to modify QP to RTS");
        return -1;
    }
    SLIME_LOG_INFO("RDMA exchange done");
    connected_ = true;

    if (ibv_req_notify_cq(cq_, 0)) {
        SLIME_LOG_ERROR("Failed to request notify for CQ");
        return -1;
    }
    return 0;
}

void RDMAContext::launch_future()
{
    cq_future_ = std::async(std::launch::async, [this]() -> void { cq_poll_handle(); });
    wq_future_ = std::async(std::launch::async, [this]() -> void { wq_dispatch_handle(); });
}

void RDMAContext::stop_future()
{
    // Stop work queue dispatch
    if (!stop_wq_future_ && wq_future_.valid()) {
        stop_wq_future_ = true;
        has_runnable_event_.notify_one();
        wq_future_.get();
    }

    if (!stop_cq_future_ && cq_future_.valid()) {
        stop_cq_future_ = true;

        // create fake wr to wake up cq thread
        ibv_req_notify_cq(cq_, 0);
        struct ibv_sge sge;
        memset(&sge, 0, sizeof(sge));
        sge.addr   = (uintptr_t)this;
        sge.length = sizeof(*this);
        sge.lkey   = 0;

        struct ibv_send_wr send_wr;
        memset(&send_wr, 0, sizeof(send_wr));
        // send_wr.wr_id      = (uintptr_t)this;
        send_wr.wr_id      = 0;
        send_wr.sg_list    = &sge;
        send_wr.num_sge    = 1;
        send_wr.opcode     = IBV_WR_SEND;
        send_wr.send_flags = IBV_SEND_SIGNALED;

        struct ibv_send_wr* bad_send_wr;
        {
            std::unique_lock<std::mutex> lock(rdma_post_send_mutex_);
            ibv_post_send(qp_, &send_wr, &bad_send_wr);
        }
        // wait thread done
        cq_future_.get();
    }
}

int RDMAContext::submit(RDMAAssignment* rdma_assignment)
{
    std::unique_lock<std::mutex> lock(assign_queue_mutex_);

    assign_queue_.push(rdma_assignment);
    has_runnable_event_.notify_one();
    return 0;
}

int64_t RDMAContext::send_async(RDMAAssignment* assign)
{
    int ret;

    struct ibv_mr* mr        = memory_pool_.get_mr(assign->batch_[0].mr_key);
    remote_mr_t    remote_mr = memory_pool_.get_remote_mr(assign->batch_[0].mr_key);

    struct ibv_sge sge;
    memset(&sge, 0, sizeof(sge));
    sge.addr   = (uintptr_t)mr->addr + assign->batch_[0].source_offset;
    sge.length = assign->batch_[0].length;
    sge.lkey   = mr->lkey;

    struct ibv_send_wr wr, *bad_wr = NULL;
    memset(&wr, 0, sizeof(wr));

    wr.wr_id      = 0;
    wr.opcode     = IBV_WR_SEND;
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.send_flags = IBV_SEND_SIGNALED;

    {
        std::unique_lock<std::mutex> lock(rdma_post_send_mutex_);
        ret = ibv_post_send(qp_, &wr, &bad_wr);
    }

    if (ret) {
        SLIME_LOG_ERROR("Failed to post RDMA send : " << strerror(ret));
        return -1;
    }

    return 0;
}

int64_t RDMAContext::recv_async(RDMAAssignment* assign)
{
    int ret;

    struct ibv_mr* mr        = memory_pool_.get_mr(assign->batch_[0].mr_key);
    remote_mr_t    remote_mr = memory_pool_.get_remote_mr(assign->batch_[0].mr_key);

    struct ibv_sge sge;
    memset(&sge, 0, sizeof(sge));
    sge.addr   = (uintptr_t)mr->addr + assign->batch_[0].source_offset;
    sge.length = assign->batch_[0].length;
    sge.lkey   = mr->lkey;

    struct ibv_recv_wr wr, *bad_wr = NULL;
    memset(&wr, 0, sizeof(wr));

    wr.wr_id   = 0;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    {
        std::unique_lock<std::mutex> lock(rdma_post_send_mutex_);
        ret = ibv_post_recv(qp_, &wr, &bad_wr);
    }

    if (ret) {
        SLIME_LOG_ERROR("Failed to post RDMA send : " << strerror(ret));
        return -1;
    }

    return 0;
}

int64_t RDMAContext::read_batch_async(RDMAAssignment* assign)
{
    size_t              batch_size = assign->batch_.size();
    struct ibv_send_wr* bad_wr     = NULL;
    struct ibv_send_wr* wr         = new ibv_send_wr[batch_size];
    struct ibv_sge*     sge        = new ibv_sge[batch_size];

    for (size_t i = 0; i < batch_size; ++i) {
        Assignment     subassign   = assign->batch_[i];
        struct ibv_mr* mr          = memory_pool_.get_mr(subassign.mr_key);
        remote_mr_t    remote_mr   = memory_pool_.get_remote_mr(subassign.mr_key);
        uint64_t       remote_addr = remote_mr.addr;
        uint32_t       remote_rkey = remote_mr.rkey;
        memset(&sge[i], 0, sizeof(ibv_sge));
        sge[i].addr   = (uint64_t)mr->addr + subassign.source_offset;
        sge[i].length = subassign.length;
        sge[i].lkey   = mr->lkey;

        wr[i].wr_id               = (i == batch_size - 1) ? (uintptr_t)(assign) : 0;
        wr[i].opcode              = IBV_WR_RDMA_READ;
        wr[i].sg_list             = &sge[i];
        wr[i].num_sge             = 1;
        wr[i].send_flags          = (i == batch_size - 1) ? IBV_SEND_SIGNALED : 0;
        wr[i].wr.rdma.remote_addr = remote_addr + assign->batch_[i].target_offset;
        wr[i].wr.rdma.rkey        = remote_rkey;
        wr[i].next                = (i == batch_size - 1) ? NULL : &wr[i + 1];
    }

    outstanding_rdma_reads_ += batch_size;

    int ret = 0;
    {
        std::unique_lock<std::mutex> lock(rdma_post_send_mutex_);
        ret = ibv_post_send(qp_, wr, &bad_wr);
    }

    delete[] wr;
    delete[] sge;

    if (ret) {
        SLIME_LOG_ERROR("Failed to post RDMA send : " << strerror(ret));
        return -1;
    }

    return 0;
}

int64_t RDMAContext::cq_poll_handle()
{
    SLIME_LOG_INFO("Polling CQ");

    if (!connected_) {
        SLIME_LOG_ERROR("Start CQ handle before connected, please construct first");
        return -1;
    }
    if (comp_channel_ == NULL)
        SLIME_LOG_ERROR("comp_channel_ should be constructed");

    while (!stop_cq_future_) {
        struct ibv_cq* ev_cq;
        void*          cq_context;

        if (ibv_get_cq_event(comp_channel_, &ev_cq, &cq_context) != 0) {
            SLIME_LOG_ERROR("Failed to get CQ event");
            return -1;
        }

        ibv_ack_cq_events(ev_cq, 1);
        if (ibv_req_notify_cq(ev_cq, 0) != 0) {
            SLIME_LOG_ERROR("Failed to request CQ notification");
            return -1;
        }

        struct ibv_wc wc[POLL_COUNT];

        while (size_t nr_poll = ibv_poll_cq(cq_, POLL_COUNT, wc)) {
            if (stop_cq_future_)
                return 0;
            if (nr_poll < 0) {
                SLIME_LOG_WARN("Worker: Failed to poll completion queues");
                continue;
            }
            for (size_t i = 0; i < nr_poll; ++i) {
                int64_t status_code;
                if (wc[i].status == IBV_WC_SUCCESS) {
                    SLIME_LOG_INFO("WR completed successfully.");
                    status_code = 200;
                }
                else {
                    SLIME_LOG_ERROR("WR failed with status: " << ibv_wc_status_str(wc[i].status) << std::endl);
                    status_code = wc[i].status;
                }
                if (wc[i].wr_id != 0) {
                    RDMAAssignment* assign = reinterpret_cast<RDMAAssignment*>(wc[i].wr_id);
                    switch (OpCode wr_type = assign->opcode_) {
                        case OpCode::READ:
                        case OpCode::SEND:
                        case OpCode::RECV:
                            assign->callback_(status_code);
                            break;
                        default:
                            SLIME_ABORT("Unimplemented WrType " << int64_t(wr_type));
                    }
                    size_t batch_size = assign->batch_.size();
                    outstanding_rdma_reads_ -= batch_size;
                }
            }
        }
    }
    return 0;
}

int64_t RDMAContext::wq_dispatch_handle()
{
    SLIME_LOG_INFO("Handling WQ");

    if (!connected_) {
        SLIME_LOG_ERROR("Start CQ handle before connected, please construct first");
        return -1;
    }

    if (comp_channel_ == NULL)
        SLIME_LOG_ERROR("comp_channel_ should be constructed");

    while (!stop_wq_future_) {
        std::unique_lock<std::mutex> lock(assign_queue_mutex_);
        has_runnable_event_.wait(lock, [this]() { return !assign_queue_.empty() || stop_wq_future_; });
        if (stop_wq_future_)
            return 0;
        while (!assign_queue_.empty()) {
            RDMAAssignment* front_assign = assign_queue_.front();
            size_t          batch_size   = front_assign->batch_.size();
            if (batch_size > MAX_SEND_WR) {
                SLIME_LOG_ERROR("batch_size(" << batch_size << ") > MAX SEND WR(" << MAX_SEND_WR
                                              << "), this request will be ignored");
                front_assign->callback_(0);
                assign_queue_.pop();
            }
            else if (batch_size + outstanding_rdma_reads_ < MAX_SEND_WR) {
                switch (front_assign->opcode_) {
                    case OpCode::SEND:
                        send_async(front_assign);
                        break;
                    case OpCode::RECV:
                        recv_async(front_assign);
                        break;
                    case OpCode::READ:
                        read_batch_async(front_assign);
                        break;
                    default:
                        SLIME_LOG_ERROR("Unknown OpCode");
                }
                assign_queue_.pop();
            }
            else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                SLIME_LOG_WARN("Assignment Queue is full.");
            }
        }
    }
    return 0;
}

}  // namespace slime
