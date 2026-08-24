#pragma once

#include <functional>
#include <string>
#include <vector>

#include "dlslime/csrc/common/json.hpp"
#include "dlslime/csrc/topology/backend.h"

namespace dlslime::topology {

using json = nlohmann::json;

class TopologyBuilder {
public:
    explicit TopologyBuilder(json document);

    json&       document() noexcept;
    const json& document() const noexcept;
    void        setBackendStatus(std::string_view name, BackendStatus status);

private:
    json document_;
};

std::string normalizeLinkTypeValue(const std::string& link_type);

json makeBaseTopology();

json discoverWithBackends(const std::vector<std::reference_wrapper<const DiscoveryBackend>>& backends);

const char* backendStatusName(BackendStatus status) noexcept;

}  // namespace dlslime::topology
