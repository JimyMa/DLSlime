#include <cassert>
#include <stdexcept>
#include <string_view>

#include "dlslime/csrc/topology/core.h"

namespace {

class FakeBackend final: public dlslime::topology::DiscoveryBackend {
public:
    std::string_view name() const noexcept override
    {
        return "fake";
    }

    dlslime::topology::BackendStatus probe() const noexcept override
    {
        return dlslime::topology::BackendStatus::Available;
    }

    void discover(dlslime::topology::TopologyBuilder& builder) const override
    {
        builder.document()["accelerators"].push_back({{"id", "gpu-0"}});
    }
};

class UnavailableBackend final: public dlslime::topology::DiscoveryBackend {
public:
    std::string_view name() const noexcept override
    {
        return "missing";
    }

    dlslime::topology::BackendStatus probe() const noexcept override
    {
        return dlslime::topology::BackendStatus::Unavailable;
    }

    void discover(dlslime::topology::TopologyBuilder&) const override
    {
        assert(false && "unavailable backends must not run discovery");
    }
};

class NotBuiltBackend final: public dlslime::topology::DiscoveryBackend {
public:
    std::string_view name() const noexcept override
    {
        return "not-built";
    }

    dlslime::topology::BackendStatus probe() const noexcept override
    {
        return dlslime::topology::BackendStatus::NotBuilt;
    }

    void discover(dlslime::topology::TopologyBuilder&) const override
    {
        assert(false && "not-built backends must not run discovery");
    }
};

class ThrowingBackend final: public dlslime::topology::DiscoveryBackend {
public:
    std::string_view name() const noexcept override
    {
        return "throwing";
    }

    dlslime::topology::BackendStatus probe() const noexcept override
    {
        return dlslime::topology::BackendStatus::Available;
    }

    void discover(dlslime::topology::TopologyBuilder& builder) const override
    {
        builder.document()["nics"].push_back({{"name", "partial"}});
        throw std::runtime_error("discovery failed");
    }
};

class TailBackend final: public dlslime::topology::DiscoveryBackend {
public:
    std::string_view name() const noexcept override
    {
        return "tail";
    }

    dlslime::topology::BackendStatus probe() const noexcept override
    {
        return dlslime::topology::BackendStatus::Available;
    }

    void discover(dlslime::topology::TopologyBuilder& builder) const override
    {
        builder.document()["memory_keys"].push_back("tail-ran");
    }
};

}  // namespace

int main()
{
    FakeBackend        fake;
    UnavailableBackend missing;
    NotBuiltBackend    not_built;
    ThrowingBackend    throwing;
    TailBackend        tail;
    const auto         topology = dlslime::topology::discoverWithBackends({fake, missing, not_built, throwing, tail});

    assert(topology.at("schema_version") == 1);
    assert(topology.at("accelerators").size() == 1);
    assert(topology.at("topology_backends").at("fake") == "AVAILABLE");
    assert(topology.at("topology_backends").at("missing") == "UNAVAILABLE");
    assert(topology.at("topology_backends").at("not-built") == "NOT_BUILT");
    assert(topology.at("topology_backends").at("throwing") == "DEGRADED");
    assert(topology.at("nics").empty());
    assert(topology.at("memory_keys") == dlslime::topology::json::array({"tail-ran"}));
    assert(dlslime::topology::normalizeLinkTypeValue(" ethernet ") == "RoCE");
    assert(dlslime::topology::normalizeLinkTypeValue("InfiniBand") == "IB");
    assert(dlslime::topology::normalizeLinkTypeValue("") == "UNKNOWN");
    return 0;
}
