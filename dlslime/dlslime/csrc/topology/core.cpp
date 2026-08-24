#include "dlslime/csrc/topology/core.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <utility>

namespace dlslime::topology {
namespace {

json localHost()
{
    std::array<char, 256> hostname_buf{};
    std::string           hostname = "localhost";
    if (gethostname(hostname_buf.data(), hostname_buf.size()) == 0) {
        hostname_buf.back() = '\0';
        hostname            = hostname_buf.data();
    }

    std::string address = hostname;
    addrinfo    hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    if (getaddrinfo(hostname.c_str(), nullptr, &hints, &result) == 0) {
        std::string first_address;
        for (addrinfo* item = result; item != nullptr; item = item->ai_next) {
            auto* addr = reinterpret_cast<sockaddr_in*>(item->ai_addr);
            char  ip[INET_ADDRSTRLEN]{};
            if (inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip)) == nullptr) {
                continue;
            }
            std::string candidate = ip;
            if (first_address.empty()) {
                first_address = candidate;
            }
            if (candidate.rfind("127.", 0) != 0) {
                address = candidate;
                break;
            }
        }
        if (address == hostname && !first_address.empty()) {
            address = first_address;
        }
        freeaddrinfo(result);
    }

    return {{"hostname", hostname}, {"address", address}};
}

int64_t nowSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

TopologyBuilder::TopologyBuilder(json document): document_(std::move(document)) {}

json& TopologyBuilder::document() noexcept
{
    return document_;
}

const json& TopologyBuilder::document() const noexcept
{
    return document_;
}

void TopologyBuilder::setBackendStatus(std::string_view name, BackendStatus status)
{
    document_["topology_backends"][std::string(name)] = backendStatusName(status);
}

const char* backendStatusName(BackendStatus status) noexcept
{
    switch (status) {
        case BackendStatus::NotBuilt:
            return "NOT_BUILT";
        case BackendStatus::Unavailable:
            return "UNAVAILABLE";
        case BackendStatus::Available:
            return "AVAILABLE";
        case BackendStatus::Degraded:
            return "DEGRADED";
    }
    return "UNAVAILABLE";
}

std::string normalizeLinkTypeValue(const std::string& link_type)
{
    auto        is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    std::string value    = link_type;
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char ch) { return !is_space(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](char ch) { return !is_space(ch); }).base(), value.end());

    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (lower == "ethernet" || lower == "roce") {
        return "RoCE";
    }
    if (lower == "infiniband" || lower == "ib") {
        return "IB";
    }
    return value.empty() ? "UNKNOWN" : value;
}

json makeBaseTopology()
{
    return {
        {"schema_version", 1},
        {"host", localHost()},
        {"nics", json::array()},
        {"accelerators", json::array()},
        {"memory_keys", json::array()},
        {"topology_epoch", nowSeconds()},
        {"topology_backends", json::object()},
    };
}

json discoverWithBackends(const std::vector<std::reference_wrapper<const DiscoveryBackend>>& backends)
{
    TopologyBuilder builder(makeBaseTopology());
    for (const DiscoveryBackend& backend : backends) {
        const BackendStatus status = backend.probe();
        builder.setBackendStatus(backend.name(), status);
        if (status != BackendStatus::Available && status != BackendStatus::Degraded) {
            continue;
        }

        TopologyBuilder candidate(builder.document());
        try {
            backend.discover(candidate);
            builder = std::move(candidate);
        }
        catch (...) {
            builder.setBackendStatus(backend.name(), BackendStatus::Degraded);
        }
    }
    return builder.document();
}

}  // namespace dlslime::topology
