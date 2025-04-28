
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

#include <gflags/gflags.h>
#include <zmq.h>
#include <zmq.hpp>

#include "engine/assignment.h"
#include "engine/rdma/rdma_assignment.h"
#include "engine/rdma/rdma_config.h"
#include "engine/rdma/rdma_scheduler.h"
#include "engine/rdma/rdma_transport.h"
#include "utils/json.hpp"
#include "utils/logging.h"

using json = nlohmann::json;
using namespace slime;

#define TERMINATE 0

DEFINE_string(mode, "", "initiator or target");

DEFINE_string(device_name, "mlx5_bond_0", "device name");
DEFINE_uint32(ib_port, 1, "device name");
DEFINE_string(link_type, "RoCE", "IB or RoCE");

DEFINE_string(target_addr, "10.130.8.139", "local endpoint");
DEFINE_int32(target_port, 23344, "local endpoint");
DEFINE_string(initiator_addr, "10.130.8.138", "remote endpoint");
DEFINE_int32(initiator_port, 24433, "local endpoint");

DEFINE_uint64(buffer_size, (2048000 * 160) + 1, "total size of data buffer");
DEFINE_uint64(block_size, 2048000, "block size");
DEFINE_uint64(batch_size, 160, "batch size");

DEFINE_uint64(duration, 10, "duration (s)");

json mr_info;

void* memory_allocate_initiator()
{
    SLIME_ASSERT(FLAGS_buffer_size > FLAGS_batch_size * FLAGS_block_size, "buffer_size < batch_size * block_size");
    void* data = (void*)malloc(FLAGS_buffer_size);
    memset(data, 0, FLAGS_buffer_size);
    return data;
}

void* memory_allocate_target()
{
    SLIME_ASSERT(FLAGS_buffer_size > FLAGS_batch_size * FLAGS_block_size, "buffer_size < batch_size * block_size");
    void* data      = (void*)malloc(FLAGS_buffer_size);
    char* byte_data = (char*)data;
    for (int64_t i = 0; i < FLAGS_buffer_size; ++i) {
        byte_data[i] = i % 128;
    }
    return data;
}

bool checkInitiatorCopied(void* data)
{
    char* byte_data = (char*)data;
    for (int64_t i = 0; i < (FLAGS_batch_size * FLAGS_block_size); ++i) {
        if (byte_data[i] != i % 128) {
            SLIME_ASSERT(false, "Transferred data at i = " << i << " not same.");
            return false;
        }
    }
    return true;
}

int target()
{
    void* data = memory_allocate_target();

    RDMAScheduler scheduler;
    scheduler.register_memory_region("buffer", (uintptr_t)data, FLAGS_buffer_size);
    std::cout << "Target registered MR" << std::endl;
    scheduler.connectRemoteNode(FLAGS_initiator_addr, FLAGS_initiator_port, FLAGS_target_port);
    std::cout << "Target connected remote" << std::endl;
    scheduler.waitRemoteTeriminate();
    return 0;
}

int initiator()
{
    void* data = memory_allocate_initiator();

    RDMAScheduler scheduler;
    scheduler.register_memory_region("buffer", (uintptr_t)data, FLAGS_buffer_size);
    std::cout << "Initiator registered MR" << std::endl;
    scheduler.connectRemoteNode(FLAGS_target_addr, FLAGS_target_port, FLAGS_initiator_port);
    std::cout << "Initiator connected remote" << std::endl;

    // 新增变量：统计相关
    uint64_t total_bytes = 0;
    uint64_t total_trips = 0;
    size_t   step        = 0;
    auto     start_time  = std::chrono::steady_clock::now();
    auto     deadline    = start_time + std::chrono::seconds(FLAGS_duration);

    while (std::chrono::steady_clock::now() < deadline) {

        AssignmentBatch        batch;

        for (int i = 0; i < FLAGS_batch_size; ++i) {
            Assignment assign = Assignment("buffer", i * FLAGS_block_size, i * FLAGS_block_size, FLAGS_block_size);
            batch.emplace_back(assign);
            SLIME_LOG_DEBUG(assign.dump());
        }

        int done = false;

        RDMASchedulerAssignment sch_assignment = scheduler.submitAssignment(batch);
        SLIME_LOG_DEBUG(sch_assignment.dump());
        sch_assignment.wait();

        total_bytes += FLAGS_batch_size * FLAGS_block_size;
        total_trips += 1;
    }

    auto   end_time   = std::chrono::steady_clock::now();
    double duration   = std::chrono::duration<double>(end_time - start_time).count();
    double throughput = total_bytes / duration / (1 << 20);  // MB/s

    std::cout << "Batch size        : " << FLAGS_batch_size << std::endl;
    std::cout << "Block size        : " << FLAGS_block_size << std::endl;

    std::cout << "Total trips       : " << total_trips << std::endl;
    std::cout << "Total transferred : " << total_bytes / (1 << 20) << " MiB" << std::endl;
    std::cout << "Duration          : " << duration << " seconds" << std::endl;
    std::cout << "Average Latency   : " << duration / total_trips * 1000 << " ms/trip" << std::endl;
    std::cout << "Throughput        : " << throughput << " MiB/s" << std::endl;

    scheduler.teriminate();

    SLIME_ASSERT(checkInitiatorCopied(data), "Transferred data not equal!");

    return 0;
}

int main(int argc, char** argv)
{
    gflags::ParseCommandLineFlags(&argc, &argv, false);
    if (FLAGS_mode == "initiator") {
        return initiator();
    }
    else if (FLAGS_mode == "target") {
        return target();
    }
    SLIME_ABORT("Unsupported mode: must be 'initiator' or 'target'");
}
