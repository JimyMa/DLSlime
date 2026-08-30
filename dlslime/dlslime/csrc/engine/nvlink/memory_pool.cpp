#include "memory_pool.h"

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "cuda_common.cuh"

namespace dlslime {

static std::string auto_name_from_ptr(uintptr_t ptr)
{
    std::ostringstream oss;
    oss << "auto_0x" << std::hex << ptr;
    return oss.str();
}

namespace {
void driverCheck(CUresult result, const char* operation)
{
    if (result == CUDA_SUCCESS)
        return;
    const char* name    = nullptr;
    const char* message = nullptr;
    cuGetErrorName(result, &name);
    cuGetErrorString(result, &message);
    throw std::runtime_error(std::string(operation) + " failed: " + (name ? name : "CUDA_ERROR") + " ("
                             + (message ? message : "unknown") + ")");
}
}  // namespace

int NVLinkMemoryPool::active_device()
{
    CUDACHECK(cudaSetDevice(device_index_ >= 0 ? device_index_ : 0));
    CUDACHECK(cudaFree(nullptr));
    int current = 0;
    CUDACHECK(cudaGetDevice(&current));
    return current;
}

NVLinkMemoryPool::~NVLinkMemoryPool()
{
    for (int32_t i = 0; i < static_cast<int32_t>(remote_handle_to_mr_.size()); ++i)
        unregister_remote_memory_region(i);
    for (int32_t i = 0; i < static_cast<int32_t>(handle_to_mr_.size()); ++i)
        unregister_memory_region(i);
}

json NVLinkMemoryPool::allocate_fabric_memory_region(size_t length, std::optional<std::string> name)
{
    if (length == 0)
        throw std::invalid_argument("Fabric allocation length must be positive");
    const int           device = active_device();
    CUmemAllocationProp prop{};
    prop.type                 = CU_MEM_ALLOCATION_TYPE_PINNED;
    prop.requestedHandleTypes = CU_MEM_HANDLE_TYPE_FABRIC;
    prop.location.type        = CU_MEM_LOCATION_TYPE_DEVICE;
    prop.location.id          = device;
    size_t granularity        = 0;
    driverCheck(cuMemGetAllocationGranularity(&granularity, &prop, CU_MEM_ALLOC_GRANULARITY_MINIMUM),
                "cuMemGetAllocationGranularity");
    const size_t                 allocation_size = ((length + granularity - 1) / granularity) * granularity;
    CUmemGenericAllocationHandle allocation{};
    CUdeviceptr                  address = 0;
    driverCheck(cuMemCreate(&allocation, allocation_size, &prop, 0), "cuMemCreate(FABRIC)");
    try {
        driverCheck(cuMemAddressReserve(&address, allocation_size, granularity, 0, 0), "cuMemAddressReserve");
        driverCheck(cuMemMap(address, allocation_size, 0, allocation, 0), "cuMemMap");
        CUmemAccessDesc access{};
        access.location = prop.location;
        access.flags    = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
        driverCheck(cuMemSetAccess(address, allocation_size, &access, 1), "cuMemSetAccess");
        nvlink_mr_t mr{};
        mr.addr              = static_cast<uintptr_t>(address);
        mr.length            = length;
        mr.allocation_size   = allocation_size;
        mr.allocation_handle = allocation;
        mr.fabric            = true;
        mr.owns_allocation   = true;
        driverCheck(cuMemExportToShareableHandle(&mr.fabric_handle, allocation, CU_MEM_HANDLE_TYPE_FABRIC, 0),
                    "cuMemExportToShareableHandle(FABRIC)");
        const int32_t     handle  = static_cast<int32_t>(handle_to_mr_.size());
        const std::string mr_name = name.value_or(auto_name_from_ptr(mr.addr));
        handle_to_mr_.push_back(mr);
        handle_to_name_.push_back(mr_name);
        ptr_to_handle_[mr.addr]  = handle;
        name_to_handle_[mr_name] = handle;
        return {{"ptr", mr.addr},
                {"handle", handle},
                {"length", length},
                {"allocation_size", allocation_size},
                {"name", mr_name}};
    }
    catch (...) {
        if (address) {
            cuMemUnmap(address, allocation_size);
            cuMemAddressFree(address, allocation_size);
        }
        cuMemRelease(allocation);
        throw;
    }
}

int32_t NVLinkMemoryPool::register_memory_region(uintptr_t                  addr,
                                                 uint64_t                   offset,
                                                 size_t                     length,
                                                 std::optional<std::string> name)
{
    if (ptr_to_handle_.count(addr)) {
        int32_t handle = ptr_to_handle_[addr];
        if (name.has_value()) {
            name_to_handle_[name.value()] = handle;
            handle_to_name_[handle]       = name.value();
        }
        return handle;
    }

    nvlink_mr_t mr{};
    mr.addr   = addr;
    mr.offset = offset;
    mr.length = length;

    // VMM allocations can be retained and re-exported by each per-peer
    // endpoint. The original allocation remains responsible for the mapping.
    CUmemGenericAllocationHandle retained{};
    CUresult                     retain_result = cuMemRetainAllocationHandle(&retained, reinterpret_cast<void*>(addr));
    if (retain_result == CUDA_SUCCESS) {
        CUmemAllocationProp prop{};
        driverCheck(cuMemGetAllocationPropertiesFromHandle(&prop, retained), "cuMemGetAllocationPropertiesFromHandle");
        if ((prop.requestedHandleTypes & CU_MEM_HANDLE_TYPE_FABRIC) != 0) {
            size_t      address_range = 0;
            CUdeviceptr base          = 0;
            driverCheck(cuMemGetAddressRange(&base, &address_range, static_cast<CUdeviceptr>(addr)),
                        "cuMemGetAddressRange");
            mr.addr = static_cast<uintptr_t>(base);
            mr.offset += addr - mr.addr;
            mr.allocation_size    = address_range;
            mr.allocation_handle  = retained;
            mr.fabric             = true;
            mr.retains_allocation = true;
            driverCheck(cuMemExportToShareableHandle(&mr.fabric_handle, retained, CU_MEM_HANDLE_TYPE_FABRIC, 0),
                        "cuMemExportToShareableHandle(FABRIC retained)");
        }
        else {
            cuMemRelease(retained);
        }
    }
    if (!mr.fabric) {
        cudaIpcMemHandle_t ipc_handle;
        CUDACHECK(cudaIpcGetMemHandle(&ipc_handle, (char*)addr));
        mr.ipc_handle = ipc_handle;
    }

    int32_t     handle  = static_cast<int32_t>(handle_to_mr_.size());
    std::string mr_name = name.value_or(auto_name_from_ptr(addr));

    handle_to_mr_.push_back(mr);
    handle_to_name_.push_back(mr_name);
    ptr_to_handle_[addr]     = handle;
    name_to_handle_[mr_name] = handle;

    return handle;
}

int32_t NVLinkMemoryPool::register_fabric_memory_region(const json& mr_info, std::optional<std::string> name)
{
    const std::string mr_name = name.value_or(mr_info.at("name").get<std::string>());
    if (name_to_handle_.count(mr_name))
        return name_to_handle_[mr_name];
    nvlink_mr_t mr{};
    mr.offset          = mr_info.value("offset", 0ULL);
    mr.length          = mr_info.at("length");
    mr.allocation_size = mr_info.at("allocation_size");
    const auto bytes   = mr_info.at("fabric_handle").get<std::vector<unsigned char>>();
    if (bytes.size() != CU_IPC_HANDLE_SIZE)
        throw std::runtime_error("Invalid CUDA Fabric handle size");
    std::copy(bytes.begin(), bytes.end(), mr.fabric_handle.data);
    const int device = active_device();
    driverCheck(cuMemImportFromShareableHandle(&mr.allocation_handle, &mr.fabric_handle, CU_MEM_HANDLE_TYPE_FABRIC),
                "cuMemImportFromShareableHandle(local FABRIC)");
    CUdeviceptr address = 0;
    try {
        driverCheck(cuMemAddressReserve(&address, mr.allocation_size, 0, 0, 0), "cuMemAddressReserve(local FABRIC)");
        driverCheck(cuMemMap(address, mr.allocation_size, 0, mr.allocation_handle, 0), "cuMemMap(local FABRIC)");
        CUmemAccessDesc access{};
        access.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
        access.location.id   = device;
        access.flags         = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
        driverCheck(cuMemSetAccess(address, mr.allocation_size, &access, 1), "cuMemSetAccess(local FABRIC)");
    }
    catch (...) {
        if (address) {
            cuMemUnmap(address, mr.allocation_size);
            cuMemAddressFree(address, mr.allocation_size);
        }
        cuMemRelease(mr.allocation_handle);
        throw;
    }
    mr.addr              = static_cast<uintptr_t>(address);
    mr.fabric            = true;
    mr.owns_allocation   = true;
    const int32_t handle = static_cast<int32_t>(handle_to_mr_.size());
    handle_to_mr_.push_back(mr);
    handle_to_name_.push_back(mr_name);
    ptr_to_handle_[mr.addr]  = handle;
    name_to_handle_[mr_name] = handle;
    return handle;
}

int32_t NVLinkMemoryPool::unregister_memory_region(int32_t handle)
{
    if (handle < 0 || static_cast<size_t>(handle) >= handle_to_mr_.size()) {
        return -1;
    }
    auto& mr = handle_to_mr_[handle];
    ptr_to_handle_.erase(mr.addr);
    if (mr.fabric && mr.owns_allocation && mr.addr) {
        cuMemUnmap(static_cast<CUdeviceptr>(mr.addr), mr.allocation_size);
        cuMemAddressFree(static_cast<CUdeviceptr>(mr.addr), mr.allocation_size);
        cuMemRelease(mr.allocation_handle);
    }
    else if (mr.retains_allocation) {
        cuMemRelease(mr.allocation_handle);
    }
    if (static_cast<size_t>(handle) < handle_to_name_.size()) {
        name_to_handle_.erase(handle_to_name_[handle]);
    }
    mr = nvlink_mr_t{};
    return 0;
}

int32_t NVLinkMemoryPool::register_remote_memory_region(const json& mr_info, std::optional<std::string> name)
{
    std::string mr_name = name.value_or(mr_info.value("name", ""));
    if (mr_name.empty()) {
        SLIME_LOG_ERROR("Remote MR registration requires a name (via parameter or JSON)");
        return -1;
    }

    if (remote_name_to_handle_.count(mr_name)) {
        return remote_name_to_handle_[mr_name];
    }

    nvlink_mr_t mr{};
    mr.offset = mr_info.value("offset", 0ULL);
    mr.length = mr_info.at("length");
    if (mr_info.value("handle_type", std::string("cuda_ipc")) == "fabric") {
        const int  device = active_device();
        const auto bytes  = mr_info.at("fabric_handle").get<std::vector<unsigned char>>();
        if (bytes.size() != CU_IPC_HANDLE_SIZE)
            throw std::runtime_error("Invalid CUDA Fabric handle size");
        std::copy(bytes.begin(), bytes.end(), mr.fabric_handle.data);
        mr.allocation_size = mr_info.at("allocation_size");
        driverCheck(cuMemImportFromShareableHandle(&mr.allocation_handle, &mr.fabric_handle, CU_MEM_HANDLE_TYPE_FABRIC),
                    "cuMemImportFromShareableHandle(FABRIC)");
        CUdeviceptr address = 0;
        try {
            driverCheck(cuMemAddressReserve(&address, mr.allocation_size, 0, 0, 0), "cuMemAddressReserve(remote)");
            driverCheck(cuMemMap(address, mr.allocation_size, 0, mr.allocation_handle, 0), "cuMemMap(remote)");
            CUmemAccessDesc access{};
            access.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
            access.location.id   = device;
            access.flags         = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
            driverCheck(cuMemSetAccess(address, mr.allocation_size, &access, 1), "cuMemSetAccess(remote)");
        }
        catch (...) {
            if (address) {
                cuMemUnmap(address, mr.allocation_size);
                cuMemAddressFree(address, mr.allocation_size);
            }
            cuMemRelease(mr.allocation_handle);
            throw;
        }
        mr.addr            = static_cast<uintptr_t>(address);
        mr.fabric          = true;
        mr.owns_allocation = true;
    }
    else {
        char* remote_ptr = nullptr;
        for (int i = 0; i < CUDA_IPC_HANDLE_SIZE; ++i)
            mr.ipc_handle.reserved[i] = mr_info.at("ipc_handle").at(i).get<char>();
        CUDACHECK(
            cudaIpcOpenMemHandle(reinterpret_cast<void**>(&remote_ptr), mr.ipc_handle, cudaIpcMemLazyEnablePeerAccess));
        mr.addr = reinterpret_cast<uintptr_t>(remote_ptr);
    }

    int32_t handle = static_cast<int32_t>(remote_handle_to_mr_.size());
    remote_handle_to_mr_.push_back(mr);
    remote_handle_to_name_.push_back(mr_name);
    remote_name_to_handle_[mr_name] = handle;

    return handle;
}

int32_t NVLinkMemoryPool::unregister_remote_memory_region(int32_t handle)
{
    if (handle < 0 || static_cast<size_t>(handle) >= remote_handle_to_mr_.size()) {
        return -1;
    }
    auto& mr = remote_handle_to_mr_[handle];
    if (mr.fabric && mr.owns_allocation && mr.addr) {
        cuMemUnmap(static_cast<CUdeviceptr>(mr.addr), mr.allocation_size);
        cuMemAddressFree(static_cast<CUdeviceptr>(mr.addr), mr.allocation_size);
        cuMemRelease(mr.allocation_handle);
    }
    else if (mr.addr) {
        cudaIpcCloseMemHandle(reinterpret_cast<void*>(mr.addr));
    }
    if (static_cast<size_t>(handle) < remote_handle_to_name_.size()) {
        remote_name_to_handle_.erase(remote_handle_to_name_[handle]);
    }
    mr = nvlink_mr_t{};
    return 0;
}

int32_t NVLinkMemoryPool::get_mr_handle(const std::string& name)
{
    auto it = name_to_handle_.find(name);
    if (it != name_to_handle_.end())
        return it->second;
    return -1;
}

int32_t NVLinkMemoryPool::get_mr_handle(uintptr_t data_ptr)
{
    auto it = ptr_to_handle_.find(data_ptr);
    if (it != ptr_to_handle_.end())
        return it->second;
    return -1;
}

int32_t NVLinkMemoryPool::get_remote_mr_handle(const std::string& name)
{
    auto it = remote_name_to_handle_.find(name);
    if (it != remote_name_to_handle_.end())
        return it->second;
    return -1;
}

const json NVLinkMemoryPool::mr_info() const
{
    json info;
    for (auto const& [name, handle] : name_to_handle_) {
        info[name] = handle_to_mr_[handle].json_info(name);
    }
    return info;
}

const json NVLinkMemoryPool::remote_mr_info() const
{
    json info;
    for (auto const& [name, handle] : remote_name_to_handle_) {
        info[name] = remote_handle_to_mr_[handle].json_info(name);
    }
    return info;
}
}  // namespace dlslime
