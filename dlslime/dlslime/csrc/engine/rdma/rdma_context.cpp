#include "rdma_context.h"

#include <infiniband/verbs.h>
#include <numa.h>
#include <poll.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "dlslime/csrc/common/jring.h"
#include "dlslime/csrc/common/pause.h"
#include "dlslime/csrc/engine/assignment.h"
#include "dlslime/csrc/engine/rdma/ibv_helper.h"
#include "dlslime/csrc/engine/rdma/memory_pool.h"
#include "dlslime/csrc/engine/rdma/rdma_assignment.h"
#include "dlslime/csrc/engine/rdma/rdma_channel.h"
#include "dlslime/csrc/engine/rdma/rdma_config.h"
#include "dlslime/csrc/engine/rdma/rdma_env.h"
#include "dlslime/csrc/engine/rdma/rdma_utils.h"
#include "dlslime/csrc/logging.h"
#include "dlslime/csrc/observability/obs.h"

namespace dlslime {

RDMAContext::~RDMAContext()
{
    stop_future();

    if (cq_)
        ibv_destroy_cq(cq_);

    if (ib_ctx_)
        ibv_close_device(ib_ctx_);

    SLIME_LOG_DEBUG("RDMAContext deconstructed")
}

int64_t RDMAContext::init(const std::string& dev_name, uint8_t ib_port, const std::string& link_type)
{
    device_name_ = dev_name;

    SLIME_LOG_INFO("Initializing RDMA Context ...");
    SLIME_LOG_DEBUG("device name: " << dev_name);
    SLIME_LOG_DEBUG("ib port: " << int{ib_port});
    SLIME_LOG_DEBUG("link type: " << link_type);

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

    if (!ib_ctx_ && num_devices > 0) {
        SLIME_LOG_WARN("Can't find or failed to open the specified device ",
                       dev_name,
                       ", try to open "
                       "the default device ",
                       (char*)ibv_get_device_name(dev_list[0]));
        ib_ctx_ = ibv_open_device(dev_list[0]);
    }

    if (!ib_ctx_) {
        SLIME_ABORT("Failed to open the default device");
    }

    struct ibv_device_attr device_attr;
    if (ibv_query_device(ib_ctx_, &device_attr) != 0)
        SLIME_LOG_ERROR("Failed to query device");

    SLIME_LOG_DEBUG("Max Memory Region:" << device_attr.max_mr);
    SLIME_LOG_DEBUG("Max Memory Region Size:" << device_attr.max_mr_size);
    SLIME_LOG_DEBUG("Max QP:" << device_attr.max_qp);
    SLIME_LOG_DEBUG("Max QP Working Request: " << device_attr.max_qp_wr);
    SLIME_LOG_DEBUG("Max CQ: " << int{device_attr.max_cq});
    SLIME_LOG_DEBUG("Max CQ Element: " << int{device_attr.max_cqe});
    SLIME_LOG_DEBUG("MAX QP RD ATOM: " << int{device_attr.max_qp_init_rd_atom});
    SLIME_LOG_DEBUG("MAX RES RD ATOM: " << int{device_attr.max_res_rd_atom});
    SLIME_LOG_DEBUG("Total ib ports: " << int{device_attr.phys_port_cnt});

    if (SLIME_MAX_RD_ATOMIC > int{device_attr.max_qp_init_rd_atom})
        SLIME_ABORT("MAX_RD_ATOMIC (" << SLIME_MAX_RD_ATOMIC << ") > device max RD ATOMIC ("
                                      << device_attr.max_qp_init_rd_atom << "), please set SLIME_MAX_RD_ATOMIC env "
                                      << "less than device max RD ATOMIC");

    struct ibv_port_attr port_attr;
    ib_port_ = ib_port;

    if (ibv_query_port(ib_ctx_, ib_port, &port_attr)) {
        ibv_close_device(ib_ctx_);
        SLIME_ABORT("Unable to query port " + std::to_string(ib_port_) + "\n");
    }

    if (port_attr.state == IBV_PORT_DOWN) {
        ibv_close_device(ib_ctx_);
        SLIME_ABORT("Device " << dev_name << ", Port " << int{ib_port_} << "is DISABLED.");
    }

    std::string requested_link_type = link_type;
    std::transform(requested_link_type.begin(), requested_link_type.end(), requested_link_type.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (requested_link_type.empty()) {
        requested_link_type = "auto";
    }
    else if (requested_link_type == "ethernet" || requested_link_type == "rocev1"
             || requested_link_type == "rocev2" || requested_link_type == "roce v2") {
        requested_link_type = "roce";
    }
    else if (requested_link_type == "infiniband") {
        requested_link_type = "ib";
    }
    if (requested_link_type != "auto" && requested_link_type != "ib" && requested_link_type != "roce") {
        SLIME_ABORT("Unsupported RDMA link type '" << link_type << "' (expected auto, IB, or RoCE)");
    }

    std::string detected_link_type;
    if (port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND) {
        detected_link_type = "IB";
        gidx_               = -1;
    }
    else if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
        detected_link_type = "RoCE";
        if (SLIME_GID_INDEX > 0)
            gidx_ = SLIME_GID_INDEX;
        else
            gidx_ = ibv_find_sgid_type(ib_ctx_, ib_port_, ibv_gid_type_custom::IBV_GID_TYPE_ROCE_V2, AF_INET);
        if (gidx_ < 0)
            SLIME_ABORT("Failed to find GID");
    }
    else {
        SLIME_ABORT("Unsupported link layer for device " << dev_name << ", port " << int{ib_port_});
    }

    if (requested_link_type != "auto" && requested_link_type != (detected_link_type == "IB" ? "ib" : "roce"))
        SLIME_ABORT("Device " << dev_name << ", port " << int{ib_port_} << " is " << detected_link_type
                               << " but link_type='" << link_type << "' was requested");

    SLIME_LOG_INFO("Detected RDMA link type ", detected_link_type, " for ", dev_name, ":", int{ib_port_});

    SLIME_LOG_DEBUG("Set GID INDEX to " << gidx_);

    lid_        = port_attr.lid;
    active_mtu_ = port_attr.active_mtu;

    /* Alloc Complete Queue (CQ) */
    SLIME_ASSERT(ib_ctx_, "init rdma context first");

    struct ibv_device_attr dev_attr;
    if (ibv_query_device(ib_ctx_, &dev_attr) != 0) {
        SLIME_ABORT("Failed to query device for max_cqe");
    }

    int actual_cq_depth = std::min(SLIME_MAX_CQ_DEPTH, dev_attr.max_cqe);
    SLIME_LOG_INFO("Creating CQ with depth: " << actual_cq_depth << " (Requested: " << SLIME_MAX_CQ_DEPTH
                                              << ", Hardware Max: " << dev_attr.max_cqe << ")");

    comp_channel_ = ibv_create_comp_channel(ib_ctx_);
    cq_           = ibv_create_cq(ib_ctx_, actual_cq_depth, NULL, comp_channel_, 0);
    SLIME_ASSERT(cq_, "create CQ failed");

    // Observability: register NIC for CQ-level error/completion counters
    if (obs::obs_enabled()) {
        obs_nic_id_ = obs::obs_register_nic(dev_name.c_str());
    }

    launch_future();
    SLIME_LOG_INFO("RDMA Context Initialized");
    return 0;
}

void RDMAContext::launch_future()
{
    cq_thread_ = std::thread([this]() -> void {
        bindToSocket(socketId(device_name_));
        cq_poll_handle();
    });
}

void RDMAContext::stop_future()
{
    if (!stop_cq_thread_ && cq_thread_.joinable()) {
        stop_cq_thread_ = true;

        // wait thread done
        cq_thread_.join();
    }
}

int64_t RDMAContext::cq_poll_handle()
{
    SLIME_LOG_INFO("Adaptive Event-Driven CQ Polling Started");

    if (comp_channel_ == NULL) {
        SLIME_LOG_ERROR("comp_channel_ must be constructed for event mode");
        return -1;
    }

    if (ibv_req_notify_cq(cq_, 0)) {
        SLIME_LOG_ERROR("Failed to request CQ notification");
        return -1;
    }

    constexpr int MAX_SPIN_COUNT = 10000;

    auto process_wcs = [&](int nr_poll, struct ibv_wc* wc) {
        for (int i = 0; i < nr_poll; ++i) {
            RDMAAssign::CALLBACK_STATUS status_code = RDMAAssign::SUCCESS;

            if (wc[i].status != IBV_WC_SUCCESS) {
                status_code = RDMAAssign::FAILED;
                if (wc[i].status != IBV_WC_WR_FLUSH_ERR) {
                    SLIME_LOG_ERROR("WR failed: ", ibv_wc_status_str(wc[i].status), ", Vendor Err: ", wc[i].vendor_err);

                    if (wc[i].wr_id != 0) {
                        RDMAAssign* assign = reinterpret_cast<RDMAAssign*>(wc[i].wr_id);
                        SLIME_LOG_ERROR("Failed WR ID: " << (void*)assign);
                    }

                    // Observability: record CQ error (real failure, not flush)
                    obs::obs_record_cq_error(obs_nic_id_);
                }
            }

            if (wc[i].wr_id != 0) {
                RDMAAssign* assign = reinterpret_cast<RDMAAssign*>(wc[i].wr_id);

                // Semantic completion accounting is owned by the
                // EndpointOpState-level callback below (exchange-guarded for
                // once-only semantics). CQ polling only records CQ-level
                // error signals.
                if (assign->callback_) {
                    assign->callback_(status_code, wc[i].imm_data);
                }
            }
        }
    };

    while (!stop_cq_thread_) {
        int spin_count = 0;

        while (spin_count < MAX_SPIN_COUNT && !stop_cq_thread_) {
            struct ibv_wc wc[SLIME_POLL_COUNT];
            int           nr_poll = ibv_poll_cq(cq_, SLIME_POLL_COUNT, wc);

            if (nr_poll > 0) {
                process_wcs(nr_poll, wc);
                spin_count = 0;
            }
            else if (nr_poll == 0) {
                spin_count++;
                machnet_pause();
            }
            else {
                SLIME_LOG_ERROR("Poll CQ failed in busy loop");
                return -1;
            }
        }

        if (stop_cq_thread_)
            break;

        if (ibv_req_notify_cq(cq_, 0)) {
            SLIME_LOG_ERROR("Failed to re-arm CQ");
            break;
        }

        struct ibv_wc wc_check[SLIME_POLL_COUNT];
        int           nr_check = ibv_poll_cq(cq_, SLIME_POLL_COUNT, wc_check);

        if (nr_check > 0) {
            process_wcs(nr_check, wc_check);
            continue;
        }
        else if (nr_check < 0) {
            SLIME_LOG_ERROR("Poll CQ failed in check phase");
            break;
        }

        struct ibv_cq* ev_cq;
        void*          cq_context;

        struct pollfd pfd;
        pfd.fd      = comp_channel_->fd;
        pfd.events  = POLLIN;
        pfd.revents = 0;

        int poll_ret = poll(&pfd, 1, 100);  // 100ms timeout
        if (poll_ret == 0) {
            continue;
        }
        else if (poll_ret < 0) {
            if (errno == EINTR)
                continue;
            SLIME_LOG_ERROR("poll() failed");
            break;
        }

        if (ibv_get_cq_event(comp_channel_, &ev_cq, &cq_context) != 0) {
            if (!stop_cq_thread_) {
                SLIME_LOG_ERROR("Failed to get CQ event");
            }
            break;
        }

        ibv_ack_cq_events(ev_cq, 1);
    }

    return 0;
}

}  // namespace dlslime
