#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "dlslime/csrc/engine/rdma/rdma_utils.h"
#include "dlslime/csrc/logging.h"
#include "rdma_context.h"

namespace dlslime {

class GlobalContextManager {
public:
    static GlobalContextManager& instance()
    {
        static GlobalContextManager instance;
        return instance;
    }

    std::shared_ptr<RDMAContext>
    get_context(const std::string& dev_name = "", int32_t ib_port = 1, const std::string& link_type = "auto")
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto nics = available_nic();
        if (nics.empty()) {
            throw std::runtime_error("No available RDMA devices");
        }
        if (ib_port < 1 || ib_port > 255) {
            throw std::invalid_argument("RDMA port must be in range 1..255, got " + std::to_string(ib_port));
        }

        std::string device = dev_name.empty() ? nics[0] : dev_name;
        if (std::find(nics.begin(), nics.end(), device) == nics.end()) {
            throw std::invalid_argument("RDMA device '" + device + "' is not available");
        }

        const RdmaLinkType requested_type = parseRdmaLinkType(link_type);
        const std::string  context_key    = device + ":" + std::to_string(ib_port);
        auto               existing       = contexts_.find(context_key);
        if (existing != contexts_.end()) {
            if (requested_type != RdmaLinkType::Auto && requested_type != existing->second->link_type()) {
                throw std::invalid_argument("Requested RDMA link type " + std::string(rdmaLinkTypeName(requested_type))
                                            + " does not match cached context link type "
                                            + rdmaLinkTypeName(existing->second->link_type()));
            }
            return existing->second;
        }

        SLIME_LOG_INFO("Initializing new RDMAContext for device: ", device);
        auto context = std::make_shared<RDMAContext>();
        context->init(device, static_cast<uint8_t>(ib_port), rdmaLinkTypeName(requested_type));
        contexts_[context_key] = context;

        if (!default_context_) {
            default_context_ = context;
        }
        return context;
    }

    std::shared_ptr<RDMAContext> get_default_context()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return default_context_;
    }

private:
    GlobalContextManager() = default;

    GlobalContextManager(const GlobalContextManager&)            = delete;
    GlobalContextManager& operator=(const GlobalContextManager&) = delete;

    std::mutex mutex_;

    std::unordered_map<std::string, std::shared_ptr<RDMAContext>> contexts_;

    std::shared_ptr<RDMAContext> default_context_ = nullptr;
};

}  // namespace dlslime
