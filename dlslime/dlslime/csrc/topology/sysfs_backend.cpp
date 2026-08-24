#include "dlslime/csrc/topology/sysfs_backend.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace dlslime::topology {

namespace {

namespace fs = std::filesystem;

std::string trim(std::string value)
{
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char ch) { return !is_space(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](char ch) { return !is_space(ch); }).base(), value.end());
    return value;
}

std::string toUpper(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

std::string readText(const fs::path& path)
{
    std::ifstream in(path);
    if (!in.is_open()) {
        throw std::runtime_error("cannot open " + path.string());
    }
    std::string value;
    std::getline(in, value);
    return trim(value);
}

std::optional<std::string> tryReadText(const fs::path& path)
{
    try {
        return readText(path);
    }
    catch (...) {
        return std::nullopt;
    }
}

std::vector<std::string> visibleDevices()
{
    const char* env = std::getenv("SLIME_VISIBLE_DEVICES");
    if (env == nullptr || std::string(env).empty()) {
        return {};
    }

    std::vector<std::string> devices;
    std::stringstream        stream(env);
    std::string              item;
    while (std::getline(stream, item, ',')) {
        item = trim(item);
        if (!item.empty()) {
            devices.push_back(item);
        }
    }
    return devices;
}

fs::path infinibandRoot(const std::string& sysfs_root)
{
    return fs::path(sysfs_root) / "class" / "infiniband";
}

fs::path deviceRoot(const std::string& sysfs_root, const std::string& device)
{
    return infinibandRoot(sysfs_root) / device;
}

std::string normalizePortState(const std::string& value)
{
    std::string state = value;
    auto        pos   = state.find(':');
    if (pos != std::string::npos) {
        state = state.substr(pos + 1);
    }
    state = toUpper(trim(state));
    return state.empty() ? "UNKNOWN" : state;
}

}  // namespace

std::vector<std::string> listRdmaDevices(const std::string& sysfs_root)
{
    std::vector<std::string> devices;
    fs::path                 root = infinibandRoot(sysfs_root);
    std::error_code          ec;
    if (!fs::exists(root, ec)) {
        return devices;
    }

    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) {
            break;
        }
        devices.push_back(entry.path().filename().string());
    }

    std::vector<std::string> visible = visibleDevices();
    if (!visible.empty()) {
        devices.erase(std::remove_if(devices.begin(),
                                     devices.end(),
                                     [&](const std::string& device) {
                                         return std::find(visible.begin(), visible.end(), device) == visible.end();
                                     }),
                      devices.end());
    }

    std::sort(devices.begin(), devices.end());
    return devices;
}

json readSysfsPort(const std::string& device, int ib_port, const std::string& sysfs_root)
{
    fs::path port_root = deviceRoot(sysfs_root, device) / "ports" / std::to_string(ib_port);

    json port = {
        {"port", ib_port},
        {"state", "UNKNOWN"},
        {"link_type", "UNKNOWN"},
    };

    if (auto value = tryReadText(port_root / "state")) {
        port["state"] = normalizePortState(*value);
    }

    if (auto value = tryReadText(port_root / "link_layer")) {
        port["link_type"] = normalizeLinkTypeValue(*value);
    }

    if (auto value = tryReadText(port_root / "active_mtu")) {
        try {
            port["active_mtu"] = std::stoi(*value);
        }
        catch (...) {
        }
    }

    return port;
}

SysfsBackend::SysfsBackend(std::optional<std::string>              preferred_device,
                           int                                     ib_port,
                           std::optional<std::string>              preferred_link_type,
                           std::string                             sysfs_root,
                           std::optional<std::vector<std::string>> devices_override):
    preferred_device_(std::move(preferred_device)),
    ib_port_(ib_port),
    preferred_link_type_(std::move(preferred_link_type)),
    sysfs_root_(std::move(sysfs_root)),
    devices_override_(std::move(devices_override))
{
}

std::string_view SysfsBackend::name() const noexcept
{
    return "sysfs";
}

BackendStatus SysfsBackend::probe() const noexcept
{
    std::error_code ec;
    return devices_override_.has_value() || fs::exists(infinibandRoot(sysfs_root_), ec) ? BackendStatus::Available :
                                                                                          BackendStatus::Unavailable;
}

void SysfsBackend::discover(TopologyBuilder& builder) const
{
    std::vector<std::string> devices = devices_override_.value_or(listRdmaDevices(sysfs_root_));

    if (preferred_device_ && !preferred_device_->empty()) {
        std::vector<std::string> ordered;
        if (std::find(devices.begin(), devices.end(), *preferred_device_) != devices.end()) {
            ordered.push_back(*preferred_device_);
        }
        for (const auto& device : devices) {
            if (device != *preferred_device_) {
                ordered.push_back(device);
            }
        }
        devices = std::move(ordered);
    }

    // Empty `devices` is not an error: it means the host has no RDMA devices
    // visible to us (e.g. TCP-only deployment, or `SLIME_VISIBLE_DEVICES`
    // filtered everything out). Return an empty topology and let the caller
    // decide whether to fall back to TCP. Mirrors the early-return at the
    // top of listRdmaDevices() when the sysfs root is absent.
    json nics = json::array();
    for (const auto& device : devices) {
        json port = readSysfsPort(device, ib_port_, sysfs_root_);
        if (port.value("link_type", "UNKNOWN") == "UNKNOWN") {
            port["link_type"] = preferred_link_type_ ? normalizeLinkTypeValue(*preferred_link_type_) : "UNKNOWN";
        }
        if (port.value("state", "UNKNOWN") == "UNKNOWN") {
            port["state"] = "ACTIVE";
        }

        int numa_node = -1;
        try {
            std::string value =
                readText(fs::weakly_canonical(deviceRoot(sysfs_root_, device) / "device") / "numa_node");
            numa_node = std::stoi(value);
        }
        catch (...) {
        }

        std::string pci_bus_id;
        try {
            pci_bus_id = fs::weakly_canonical(deviceRoot(sysfs_root_, device) / "device").filename().string();
        }
        catch (...) {
        }

        nics.push_back({
            {"name", device},
            {"health", port.value("state", "ACTIVE") == "ACTIVE" ? "AVAILABLE" : "DEGRADED"},
            {"numa_node", numa_node},
            {"pci_bus_id", pci_bus_id},
            {"ports", json::array({port})},
        });
    }

    builder.document()["nics"] = std::move(nics);
}

}  // namespace dlslime::topology
