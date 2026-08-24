#pragma once

#include <infiniband/verbs.h>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dlslime/csrc/common/json.hpp"
#include "rdma_assignment.h"

namespace dlslime {

class RDMAChannel;

using json = nlohmann::json;

enum class RdmaLinkType {
    Auto,
    InfiniBand,
    RoCE,
};

RdmaLinkType parseRdmaLinkType(const std::string& value);
RdmaLinkType detectRdmaLinkType(uint8_t link_layer);
RdmaLinkType resolveRdmaLinkType(const std::string& requested, uint8_t detected_link_layer);
const char*  rdmaLinkTypeName(RdmaLinkType link_type);

class RDMAContext: public std::enable_shared_from_this<RDMAContext> {

    friend class RDMAChannel;
    friend class RDMAEndpoint;
    friend class RDMAMsgEndpoint;
    friend class RDMAIOEndpoint;
    friend class RDMAMemoryPool;

public:
    /*
      A context of rdma QP.
    */

    RDMAContext() = default;

    ~RDMAContext();

    /* Initialize */
    int64_t init(const std::string& dev_name, uint8_t ib_port, const std::string& link_type);

    void launch_future();
    void stop_future();

    RdmaLinkType link_type() const
    {
        return link_type_;
    }

    std::string get_dev_ib() const
    {
        return "@" + device_name_ + "#" + std::to_string(ib_port_);
    }

    bool validate_assignment()
    {
        // TODO: validate if the assignment is valid
        return true;
    }

private:
    inline static constexpr int      UNDEFINED_QPI            = -1;
    inline static constexpr uint32_t UNDEFINED_IMM_DATA       = -1;
    inline static constexpr uint32_t BACKPRESSURE_BUFFER_SIZE = 8192;

    std::string device_name_ = "";

    /* RDMA Configuration */
    struct ibv_context*      ib_ctx_       = nullptr;
    struct ibv_comp_channel* comp_channel_ = nullptr;
    struct ibv_cq*           cq_           = nullptr;
    uint8_t                  ib_port_      = -1;
    RdmaLinkType             link_type_    = RdmaLinkType::Auto;
    size_t                   max_num_inline_data_{0};
    uint16_t                 lid_;
    enum ibv_mtu             active_mtu_;
    union ibv_gid            gid_;
    int64_t                  gidx_;

    int32_t num_qp_;
    int32_t last_qp_selection_{-1};

    std::vector<int> select_qpi(int num)
    {
        std::vector<int> agg_qpi;
        // Simplest round robin, we could enrich it in the future

        for (int i = 0; i < num; ++i) {
            last_qp_selection_ = (last_qp_selection_ + 1) % num_qp_;
            agg_qpi.push_back(last_qp_selection_);
        }

        return agg_qpi;
    }

    typedef struct cq_management {
        // TODO: multi cq handlers.
    } cq_management_t;

    /* async cq handler */
    std::thread       cq_thread_;
    std::atomic<bool> stop_cq_thread_{false};

    /* Completion Queue Polling */
    int64_t cq_poll_handle();

    int obs_nic_id_{-1};  // Observability: registered NIC slot index

    int64_t service_level_{0};
};

}  // namespace dlslime
