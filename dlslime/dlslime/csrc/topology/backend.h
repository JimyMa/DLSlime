#pragma once

#include <string_view>

namespace dlslime::topology {

class TopologyBuilder;

enum class BackendStatus {
    NotBuilt,
    Unavailable,
    Available,
    Degraded,
};

class DiscoveryBackend {
public:
    virtual ~DiscoveryBackend() = default;

    virtual std::string_view name() const noexcept                    = 0;
    virtual BackendStatus    probe() const noexcept                   = 0;
    virtual void             discover(TopologyBuilder& builder) const = 0;
};

}  // namespace dlslime::topology
