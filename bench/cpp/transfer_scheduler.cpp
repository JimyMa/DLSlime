
#include <cassert>
#include <cstdlib>
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
DEFINE_string(link_type, "Ethernet", "IB or Ethernet");

DEFINE_string(local_addr, "10.130.8.138", "local endpoint");
DEFINE_int32(local_port, 33344, "local endpoint");
DEFINE_string(remote_addr, "10.130.8.140", "remote endpoint");
DEFINE_int32(remote_port, 44433, "local endpoint");


DEFINE_uint64(buffer_size, 1ull << 30, "total size of data buffer");
DEFINE_uint64(block_size, 32768, "block size");
DEFINE_uint64(batch_size, 80, "batch size");

DEFINE_uint64(duration, 10, "duration (s)");

json mr_info;

void* memory_allocate()
{
    SLIME_ASSERT(FLAGS_buffer_size > FLAGS_batch_size * FLAGS_block_size, "buffer_size < batch_size * block_size");
    void* data = (void*)malloc(FLAGS_buffer_size);
    return data;
}


int target()
{
    RDMAScheduler scheduler;
    void* data = memory_allocate();
    scheduler.register_memory_region("buffer", (uintptr_t)data, FLAGS_buffer_size);
    scheduler.connectRemoteNode(FLAGS_local_addr, FLAGS_local_port, FLAGS_remote_port);
    scheduler.waitRemoteTeriminate();
    return 0;
}

int initiator()
{
    void* data = memory_allocate();
    
    RDMAScheduler scheduler;
    scheduler.register_memory_region("buffer", (uintptr_t)data, FLAGS_buffer_size);
    scheduler.connectRemoteNode(FLAGS_local_addr, FLAGS_local_port, FLAGS_remote_port);


    // 新增变量：统计相关
    uint64_t total_bytes = 0;
    uint64_t total_trips = 0;
    size_t   step        = 0;
    auto     start_time  = std::chrono::steady_clock::now();
    auto     deadline    = start_time + std::chrono::seconds(FLAGS_duration);

    while (std::chrono::steady_clock::now() < deadline) {

        std::vector<uintptr_t> target_offsets, source_offsets;

        for (int i = 0; i < FLAGS_batch_size; ++i) {
            source_offsets.emplace_back(i * FLAGS_block_size);
            target_offsets.emplace_back(i * FLAGS_block_size);
        }

        int done = false;
        scheduler.submitAssignment(
            Assignment(OpCode::READ, "buffer", target_offsets, source_offsets, FLAGS_block_size, [&done](int code) {
                done = true;
            }));

        while (!done) {}
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
