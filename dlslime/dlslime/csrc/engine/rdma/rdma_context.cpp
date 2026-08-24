#include "rdma_context.h"

#include <infiniband/verbs.h>
#include <numa.h>
#include <poll.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
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

RdmaLinkType parseRdmaLinkType(const std::string& value)
{
    const auto  first      = value.find_first_not_of(" \t\r\n");
    const auto  last       = value.find_last_not_of(" \t\r\n");
    std::string normalized = first == std::string::npos ? "" : value.substr(first, last - first + 1);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (normalized.empty() || normalized == "auto") {
        return RdmaLinkType::Auto;
    }
    if (normalized == "ib" || normalized == "infiniband") {
        return RdmaLinkType::InfiniBand;
    }
    if (normalized == "roce" || normalized == "ethernet" || normalized == "rocev1" || normalized == "roce v1"
        || normalized == "rocev2" || normalized == "roce v2") {
        return RdmaLinkType::RoCE;
    }
    throw std::invalid_argument("Unsupported RDMA link type '" + value + "' (expected auto, IB, or RoCE)");
}

RdmaLinkType detectRdmaLinkType(uint8_t link_layer)
{
    if (link_layer == IBV_LINK_LAYER_INFINIBAND) {
        return RdmaLinkType::InfiniBand;
    }
    if (link_layer == IBV_LINK_LAYER_ETHERNET) {
        return RdmaLinkType::RoCE;
    }
    throw std::runtime_error("Unsupported RDMA port link layer: " + std::to_string(link_layer));
}

RdmaLinkType resolveRdmaLinkType(const std::string& requested, uint8_t detected_link_layer)
{
    const RdmaLinkType requested_type = parseRdmaLinkType(requested);
    const RdmaLinkType detected_type  = detectRdmaLinkType(detected_link_layer);
    if (requested_type != RdmaLinkType::Auto && requested_type != detected_type) {
        throw std::invalid_argument("Requested RDMA link type " + std::string(rdmaLinkTypeName(requested_type))
                                    + " does not match detected " + rdmaLinkTypeName(detected_type));
    }
    return detected_type;
}

const char* rdmaLinkTypeName(RdmaLinkType link_type)
{
    switch (link_type) {
        case RdmaLinkType::Auto:
            return "auto";
        case RdmaLinkType::InfiniBand:
            return "IB";
        case RdmaLinkType::RoCE:
            return "RoCE";
    }
    throw std::invalid_argument("Unknown RDMA link type enum value");
}

RDMAContext::~RDMAContext()
{
    stop_future();

    if (cq_)
        ibv_destroy_cq(cq_);

    if (comp_channel_)
        ibv_destroy_comp_channel(comp_channel_);

    if (ib_ctx_)
        ibv_close_device(ib_ctx_);

    SLIME_LOG_DEBUG("RDMAContext deconstructed")
}

int64_t RDMAContext::init(const std::string& dev_name, uint8_t ib_port, const std::string& link_type)
{
    if (ib_ctx_ || comp_channel_ || cq_ || cq_thread_.joinable()) {
        throw std::logic_error("RDMAContext is already initialized");
    }

    SLIME_LOG_INFO("Initializing RDMA Context ...");
    SLIME_LOG_DEBUG("device name: " << dev_name);
    SLIME_LOG_DEBUG("ib port: " << int{ib_port});
    SLIME_LOG_DEBUG("link type: " << link_type);

    struct ibv_device** dev_list = nullptr;
    int                 num_devices{0};
    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list) {
        throw std::runtime_error("Failed to get RDMA devices list");
    }
    std::unique_ptr<ibv_device*, decltype(&ibv_free_device_list)> device_list_guard(dev_list, &ibv_free_device_list);

    if (num_devices == 0) {
        throw std::runtime_error("No RDMA devices found");
    }

    struct ibv_device* ib_dev = nullptr;
    for (int i = 0; i < num_devices; ++i) {
        const char* listed_name = ibv_get_device_name(dev_list[i]);
        if (listed_name && dev_name == listed_name) {
            ib_dev = dev_list[i];
            break;
        }
    }
    if (!ib_dev) {
        throw std::invalid_argument("RDMA device '" + dev_name + "' is not available");
    }

    ib_ctx_ = ibv_open_device(ib_dev);
    if (!ib_ctx_) {
        throw std::runtime_error("Failed to open RDMA device '" + dev_name + "'");
    }
    device_name_ = ibv_get_device_name(ib_dev);

    struct ibv_device_attr device_attr {};
    if (ibv_query_device(ib_ctx_, &device_attr) != 0) {
        throw std::runtime_error("Failed to query RDMA device '" + device_name_ + "'");
    }

    SLIME_LOG_DEBUG("Max Memory Region:" << device_attr.max_mr);
    SLIME_LOG_DEBUG("Max Memory Region Size:" << device_attr.max_mr_size);
    SLIME_LOG_DEBUG("Max QP:" << device_attr.max_qp);
    SLIME_LOG_DEBUG("Max QP Working Request: " << device_attr.max_qp_wr);
    SLIME_LOG_DEBUG("Max CQ: " << int{device_attr.max_cq});
    SLIME_LOG_DEBUG("Max CQ Element: " << int{device_attr.max_cqe});
    SLIME_LOG_DEBUG("MAX QP RD ATOM: " << int{device_attr.max_qp_init_rd_atom});
    SLIME_LOG_DEBUG("MAX RES RD ATOM: " << int{device_attr.max_res_rd_atom});
    SLIME_LOG_DEBUG("Total ib ports: " << int{device_attr.phys_port_cnt});

    if (SLIME_MAX_RD_ATOMIC > int{device_attr.max_qp_init_rd_atom}) {
        throw std::runtime_error("SLIME_MAX_RD_ATOMIC=" + std::to_string(SLIME_MAX_RD_ATOMIC)
                                 + " exceeds device maximum " + std::to_string(device_attr.max_qp_init_rd_atom));
    }

    if (ib_port < 1 || ib_port > device_attr.phys_port_cnt) {
        throw std::invalid_argument("RDMA port " + std::to_string(ib_port) + " is outside valid range 1.."
                                    + std::to_string(device_attr.phys_port_cnt) + " for device " + device_name_);
    }
    ib_port_ = static_cast<uint8_t>(ib_port);

    struct ibv_port_attr port_attr {};
    if (ibv_query_port(ib_ctx_, ib_port_, &port_attr) != 0) {
        throw std::runtime_error("Unable to query RDMA port " + std::to_string(ib_port_) + " on " + device_name_);
    }
    if (port_attr.state != IBV_PORT_ACTIVE) {
        throw std::runtime_error("RDMA device " + device_name_ + ", port " + std::to_string(ib_port_)
                                 + " is not ACTIVE");
    }

    link_type_ = resolveRdmaLinkType(link_type, static_cast<uint8_t>(port_attr.link_layer));
    if (link_type_ == RdmaLinkType::InfiniBand) {
        gidx_ = -1;
    }
    else {
        if (SLIME_GID_INDEX > 0)
            gidx_ = SLIME_GID_INDEX;
        else
            gidx_ = ibv_find_sgid_type(ib_ctx_, ib_port_, ibv_gid_type_custom::IBV_GID_TYPE_ROCE_V2, AF_INET);
        if (gidx_ < 0) {
            throw std::runtime_error("Failed to find a RoCE v2 GID for " + device_name_ + ", port "
                                     + std::to_string(ib_port_));
        }
    }

    SLIME_LOG_INFO("Detected RDMA link type ", rdmaLinkTypeName(link_type_), " for ", device_name_, ":", int{ib_port_});

    SLIME_LOG_DEBUG("Set GID INDEX to " << gidx_);

    lid_        = port_attr.lid;
    active_mtu_ = port_attr.active_mtu;

    int actual_cq_depth = std::min(SLIME_MAX_CQ_DEPTH, device_attr.max_cqe);
    SLIME_LOG_INFO("Creating CQ with depth: " << actual_cq_depth << " (Requested: " << SLIME_MAX_CQ_DEPTH
                                              << ", Hardware Max: " << device_attr.max_cqe << ")");

    comp_channel_ = ibv_create_comp_channel(ib_ctx_);
    if (!comp_channel_) {
        throw std::runtime_error("Failed to create RDMA completion channel for " + device_name_);
    }
    cq_ = ibv_create_cq(ib_ctx_, actual_cq_depth, NULL, comp_channel_, 0);
    if (!cq_) {
        throw std::runtime_error("Failed to create RDMA completion queue for " + device_name_);
    }

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
