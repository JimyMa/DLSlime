#pragma once
#include <cstdint>

#include "cuda_common.cuh"
#include "dlslime/csrc/device/device_future.h"

namespace dlslime {

/** Future for one or more NVLink copies recorded on a CUDA stream. */
class NVLinkFuture: public DeviceFuture {
public:
    explicit NVLinkFuture(cudaStream_t stream)
    {
        CUDACHECK(cudaEventCreateWithFlags(&event_, cudaEventDisableTiming));
        CUDACHECK(cudaEventRecord(event_, stream));
    }

    ~NVLinkFuture() override
    {
        if (event_ != nullptr)
            cudaEventDestroy(event_);
    }

    NVLinkFuture(const NVLinkFuture&)            = delete;
    NVLinkFuture& operator=(const NVLinkFuture&) = delete;

    int32_t wait() const override
    {
        return static_cast<int32_t>(cudaEventSynchronize(event_));
    }

private:
    cudaEvent_t event_{nullptr};
};
}  // namespace dlslime
