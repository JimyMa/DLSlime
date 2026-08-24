#include <cuda.h>
#include <dlfcn.h>
#include <nvml.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "dlslime/csrc/topology/cuda_backend.h"

namespace dlslime::topology {
namespace {

namespace fs = std::filesystem;

template<typename T>
T loadSymbol(void* handle, const char* name, bool required = true)
{
    auto symbol = reinterpret_cast<T>(dlsym(handle, name));
    if (!symbol && required) {
        throw std::runtime_error(std::string("missing runtime symbol ") + name);
    }
    return symbol;
}

class SharedLibrary {
public:
    explicit SharedLibrary(const char* name): handle_(dlopen(name, RTLD_LOCAL | RTLD_NOW))
    {
        if (!handle_) {
            throw std::runtime_error(std::string("cannot load ") + name);
        }
    }

    ~SharedLibrary()
    {
        if (handle_) {
            dlclose(handle_);
        }
    }

    SharedLibrary(const SharedLibrary&)            = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;

    void* get() const noexcept
    {
        return handle_;
    }

private:
    void* handle_{nullptr};
};

struct CudaApi {
    SharedLibrary               library{"libcuda.so.1"};
    decltype(&cuInit)           init = loadSymbol<decltype(init)>(library.get(), "cuInit");
    decltype(&cuDeviceGetCount) device_get_count =
        loadSymbol<decltype(device_get_count)>(library.get(), "cuDeviceGetCount");
    decltype(&cuDeviceGet)     device_get = loadSymbol<decltype(device_get)>(library.get(), "cuDeviceGet");
    decltype(&cuDeviceGetName) device_get_name =
        loadSymbol<decltype(device_get_name)>(library.get(), "cuDeviceGetName");
    decltype(&cuDeviceGetUuid_v2) device_get_uuid =
        loadSymbol<decltype(device_get_uuid)>(library.get(), "cuDeviceGetUuid_v2", false);
    decltype(&cuDeviceGetPCIBusId) device_get_pci =
        loadSymbol<decltype(device_get_pci)>(library.get(), "cuDeviceGetPCIBusId");
    decltype(&cuDeviceGetAttribute) device_get_attribute =
        loadSymbol<decltype(device_get_attribute)>(library.get(), "cuDeviceGetAttribute");
    decltype(&cuDeviceCanAccessPeer) device_can_access_peer =
        loadSymbol<decltype(device_can_access_peer)>(library.get(), "cuDeviceCanAccessPeer");
    decltype(&cuDeviceGetP2PAttribute) device_get_p2p_attribute =
        loadSymbol<decltype(device_get_p2p_attribute)>(library.get(), "cuDeviceGetP2PAttribute", false);
    CudaApi()
    {
        if (!device_get_uuid) {
            device_get_uuid = loadSymbol<decltype(device_get_uuid)>(library.get(), "cuDeviceGetUuid", false);
        }
        if (!device_get_uuid) {
            throw std::runtime_error("missing runtime symbol cuDeviceGetUuid_v2/cuDeviceGetUuid");
        }
    }
};

struct NvmlApi {
    SharedLibrary                        library{"libnvidia-ml.so.1"};
    decltype(&nvmlInit_v2)               init     = loadSymbol<decltype(init)>(library.get(), "nvmlInit_v2");
    decltype(&nvmlShutdown)              shutdown = loadSymbol<decltype(shutdown)>(library.get(), "nvmlShutdown");
    decltype(&nvmlDeviceGetHandleByUUID) device_by_uuid =
        loadSymbol<decltype(device_by_uuid)>(library.get(), "nvmlDeviceGetHandleByUUID");
    decltype(&nvmlDeviceGetHandleByPciBusId_v2) device_by_pci =
        loadSymbol<decltype(device_by_pci)>(library.get(), "nvmlDeviceGetHandleByPciBusId_v2", false);
    decltype(&nvmlDeviceGetGpuFabricInfoV) fabric_info =
        loadSymbol<decltype(fabric_info)>(library.get(), "nvmlDeviceGetGpuFabricInfoV", false);
    decltype(&nvmlDeviceGetNvLinkState) nvlink_state =
        loadSymbol<decltype(nvlink_state)>(library.get(), "nvmlDeviceGetNvLinkState", false);
    decltype(&nvmlDeviceGetNvLinkRemoteDeviceType) nvlink_remote_type =
        loadSymbol<decltype(nvlink_remote_type)>(library.get(), "nvmlDeviceGetNvLinkRemoteDeviceType", false);
    decltype(&nvmlDeviceGetNvLinkRemotePciInfo_v2) nvlink_remote_pci =
        loadSymbol<decltype(nvlink_remote_pci)>(library.get(), "nvmlDeviceGetNvLinkRemotePciInfo_v2", false);

    NvmlApi()
    {
        const nvmlReturn_t result = init();
        if (result != NVML_SUCCESS) {
            throw std::runtime_error("nvmlInit_v2 failed");
        }
        initialized_ = true;
    }

    ~NvmlApi()
    {
        if (initialized_) {
            shutdown();
        }
    }

private:
    bool initialized_{false};
};

std::string formatUuid(const unsigned char* bytes)
{
    char output[37]{};
    std::snprintf(output,
                  sizeof(output),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  bytes[0],
                  bytes[1],
                  bytes[2],
                  bytes[3],
                  bytes[4],
                  bytes[5],
                  bytes[6],
                  bytes[7],
                  bytes[8],
                  bytes[9],
                  bytes[10],
                  bytes[11],
                  bytes[12],
                  bytes[13],
                  bytes[14],
                  bytes[15]);
    return output;
}

std::string cudaUuid(const CUuuid& uuid)
{
    return "GPU-" + formatUuid(reinterpret_cast<const unsigned char*>(uuid.bytes));
}

std::string lowerPci(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

int readNumaNode(const std::string& pci_bus_id)
{
    std::ifstream input(fs::path("/sys/bus/pci/devices") / lowerPci(pci_bus_id) / "numa_node");
    int           value = -1;
    if (input >> value) {
        return value;
    }
    return -1;
}

std::string fabricState(nvmlGpuFabricState_t state)
{
    switch (state) {
        case NVML_GPU_FABRIC_STATE_NOT_SUPPORTED:
            return "NOT_SUPPORTED";
        case NVML_GPU_FABRIC_STATE_NOT_STARTED:
            return "NOT_STARTED";
        case NVML_GPU_FABRIC_STATE_IN_PROGRESS:
            return "IN_PROGRESS";
        case NVML_GPU_FABRIC_STATE_COMPLETED:
            return "COMPLETED";
        default:
            return "UNKNOWN";
    }
}

std::string fabricHealth(unsigned char health)
{
    switch (health) {
        case NVML_GPU_FABRIC_HEALTH_SUMMARY_HEALTHY:
            return "HEALTHY";
        case NVML_GPU_FABRIC_HEALTH_SUMMARY_UNHEALTHY:
            return "UNHEALTHY";
        case NVML_GPU_FABRIC_HEALTH_SUMMARY_LIMITED_CAPACITY:
            return "LIMITED_CAPACITY";
        default:
            return "NOT_SUPPORTED";
    }
}

std::string remoteType(nvmlIntNvLinkDeviceType_t type)
{
    switch (type) {
        case NVML_NVLINK_DEVICE_TYPE_GPU:
            return "gpu";
        case NVML_NVLINK_DEVICE_TYPE_SWITCH:
            return "switch";
        case NVML_NVLINK_DEVICE_TYPE_IBMNPU:
            return "ibm_npu";
        default:
            return "unknown";
    }
}

std::pair<bool, std::vector<int>> imexChannels()
{
    std::vector<int> ids;
    std::error_code  ec;
    const fs::path   root("/dev/nvidia-caps-imex-channels");
    if (!fs::exists(root, ec)) {
        return {false, ids};
    }
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) {
            break;
        }
        const std::string name = entry.path().filename().string();
        if (name.rfind("channel", 0) != 0 || access(entry.path().c_str(), R_OK) != 0) {
            continue;
        }
        try {
            ids.push_back(std::stoi(name.substr(7)));
        }
        catch (...) {
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return {!ids.empty(), ids};
}

class DynamicCudaDiscoverySource final: public CudaDiscoverySource {
public:
    BackendStatus probe() noexcept override
    {
        try {
            CudaApi cuda;
            int     count = 0;
            if (cuda.init(0) != CUDA_SUCCESS || cuda.device_get_count(&count) != CUDA_SUCCESS) {
                return BackendStatus::Unavailable;
            }
            return BackendStatus::Available;
        }
        catch (...) {
            return BackendStatus::Unavailable;
        }
    }

    CudaTopologySnapshot snapshot() override
    {
        CudaTopologySnapshot result;
        CudaApi              cuda;
        int                  count = 0;
        if (cuda.init(0) != CUDA_SUCCESS || cuda.device_get_count(&count) != CUDA_SUCCESS) {
            return result;
        }
        result.cuda_status = BackendStatus::Available;

        std::vector<CUdevice> cuda_devices;
        for (int ordinal = 0; ordinal < count; ++ordinal) {
            CUdevice handle{};
            if (cuda.device_get(&handle, ordinal) != CUDA_SUCCESS) {
                result.cuda_status = BackendStatus::Degraded;
                continue;
            }

            CudaDeviceFact device;
            device.device_index = ordinal;
            char   name[256]{};
            char   pci[32]{};
            CUuuid uuid{};
            int    major  = 0;
            int    minor  = 0;
            int    ipc    = 0;
            int    fabric = 0;

            const bool identity_ok = cuda.device_get_name(name, sizeof(name), handle) == CUDA_SUCCESS
                                     && cuda.device_get_uuid(&uuid, handle) == CUDA_SUCCESS
                                     && cuda.device_get_pci(pci, sizeof(pci), handle) == CUDA_SUCCESS;
            if (!identity_ok) {
                result.cuda_status = BackendStatus::Degraded;
                continue;
            }

            const bool attributes_ok =
                cuda.device_get_attribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, handle) == CUDA_SUCCESS
                && cuda.device_get_attribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, handle)
                       == CUDA_SUCCESS
                && cuda.device_get_attribute(&ipc, CU_DEVICE_ATTRIBUTE_UNIFIED_ADDRESSING, handle) == CUDA_SUCCESS
                && cuda.device_get_attribute(&fabric, CU_DEVICE_ATTRIBUTE_HANDLE_TYPE_FABRIC_SUPPORTED, handle)
                       == CUDA_SUCCESS;

            device.name                    = name;
            device.uuid                    = cudaUuid(uuid);
            device.pci_bus_id              = lowerPci(pci);
            device.numa_node               = readNumaNode(device.pci_bus_id);
            device.compute_major           = major;
            device.compute_minor           = minor;
            device.unified_addressing      = ipc != 0;
            device.fabric_handle_supported = fabric != 0;
            device.degraded                = !attributes_ok;
            if (!attributes_ok) {
                result.cuda_status = BackendStatus::Degraded;
            }
            result.devices.push_back(device);
            cuda_devices.push_back(handle);
        }

        for (size_t source = 0; source < cuda_devices.size(); ++source) {
            for (size_t target = 0; target < cuda_devices.size(); ++target) {
                if (source == target) {
                    continue;
                }
                CudaP2pFact edge;
                edge.source_uuid     = result.devices[source].uuid;
                edge.target_uuid     = result.devices[target].uuid;
                int access_supported = 0;
                if (cuda.device_can_access_peer(&access_supported, cuda_devices[source], cuda_devices[target])
                    != CUDA_SUCCESS) {
                    result.cuda_status = BackendStatus::Degraded;
                    continue;
                }
                edge.access_supported = access_supported != 0;
                if (cuda.device_get_p2p_attribute) {
                    int value = 0;
                    if (cuda.device_get_p2p_attribute(&value,
                                                      CU_DEVICE_P2P_ATTRIBUTE_PERFORMANCE_RANK,
                                                      cuda_devices[source],
                                                      cuda_devices[target])
                        == CUDA_SUCCESS) {
                        edge.performance_rank = value;
                    }
                    if (cuda.device_get_p2p_attribute(&value,
                                                      CU_DEVICE_P2P_ATTRIBUTE_NATIVE_ATOMIC_SUPPORTED,
                                                      cuda_devices[source],
                                                      cuda_devices[target])
                        == CUDA_SUCCESS) {
                        edge.native_atomic_supported = value != 0;
                    }
                }
                result.p2p.push_back(std::move(edge));
            }
        }

        auto [imex_available, channel_ids] = imexChannels();
        result.imex_channel_available      = imex_available;
        result.imex_channel_ids            = std::move(channel_ids);

        try {
            NvmlApi nvml;
            result.nvml_status = BackendStatus::Available;
            for (auto& device : result.devices) {
                nvmlDevice_t nvml_device{};
                nvmlReturn_t lookup = nvml.device_by_uuid(device.uuid.c_str(), &nvml_device);
                if (lookup != NVML_SUCCESS && nvml.device_by_pci) {
                    lookup                   = nvml.device_by_pci(device.pci_bus_id.c_str(), &nvml_device);
                    device.nvml_parent_scope = lookup == NVML_SUCCESS;
                }
                if (lookup != NVML_SUCCESS) {
                    device.degraded    = true;
                    result.nvml_status = BackendStatus::Degraded;
                    continue;
                }
                if (device.nvml_parent_scope) {
                    continue;
                }

                if (nvml.fabric_info) {
                    nvmlGpuFabricInfoV_t info{};
                    info.version = nvmlGpuFabricInfo_v3;
                    if (nvml.fabric_info(nvml_device, &info) == NVML_SUCCESS
                        && info.state != NVML_GPU_FABRIC_STATE_NOT_SUPPORTED) {
                        device.fabric = MnnvlFabricFact{
                            formatUuid(info.clusterUuid),
                            info.cliqueId,
                            fabricState(info.state),
                            info.status == NVML_SUCCESS ? "SUCCESS" : "ERROR",
                            fabricHealth(info.healthSummary),
                        };
                    }
                }

                if (!nvml.nvlink_state) {
                    continue;
                }
                for (unsigned int index = 0; index < NVML_NVLINK_MAX_LINKS; ++index) {
                    nvmlEnableState_t  active{};
                    const nvmlReturn_t state_result = nvml.nvlink_state(nvml_device, index, &active);
                    if (state_result == NVML_ERROR_NOT_SUPPORTED || state_result == NVML_ERROR_INVALID_ARGUMENT) {
                        continue;
                    }
                    if (state_result != NVML_SUCCESS) {
                        result.nvml_status = BackendStatus::Degraded;
                        continue;
                    }

                    NvLinkFact link;
                    link.gpu_uuid   = device.uuid;
                    link.link_index = index;
                    link.active     = active == NVML_FEATURE_ENABLED;
                    if (nvml.nvlink_remote_type) {
                        nvmlIntNvLinkDeviceType_t type = NVML_NVLINK_DEVICE_TYPE_UNKNOWN;
                        if (nvml.nvlink_remote_type(nvml_device, index, &type) == NVML_SUCCESS) {
                            link.remote_type = remoteType(type);
                        }
                    }
                    if (link.active && link.remote_type == "gpu" && nvml.nvlink_remote_pci) {
                        nvmlPciInfo_t pci{};
                        if (nvml.nvlink_remote_pci(nvml_device, index, &pci) == NVML_SUCCESS) {
                            link.remote_pci_bus_id = lowerPci(pci.busId);
                        }
                    }
                    result.nvlink.push_back(std::move(link));
                }
            }
        }
        catch (...) {
            result.nvml_status = BackendStatus::Unavailable;
        }

        return result;
    }
};

}  // namespace

std::shared_ptr<CudaDiscoverySource> makeCudaDiscoverySource()
{
    return std::make_shared<DynamicCudaDiscoverySource>();
}

}  // namespace dlslime::topology
