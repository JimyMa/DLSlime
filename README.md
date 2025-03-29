# Slime Transfer Engine

## Usage
```python
import asyncio  # For asynchronous operations

import torch  # For GPU tensor management

from slime import avaliable_nic, RDMAEndpoint  # RDMA endpoint management


devices = avaliable_nic()
assert devices, "No RDMA devices."

# Initialize RDMA endpoint on NIC 'mlx5_bond_1' port 1 using Ethernet transport
initiator = RDMAEndpoint(device_name=devices[0], ib_port=1, link_type="Ethernet")
# Create a zero-initialized CUDA tensor on GPU 0 as local buffer
local_tensor = torch.zeros([16], device="cuda:0", dtype=torch.uint8)
# Register local GPU memory with RDMA subsystem
initiator.register_memory_region(
    mr_identifier="buffer",
    virtual_address=local_tensor.data_ptr(),
    length_bytes=local_tensor.numel() * local_tensor.itemsize,
)


# Initialize target endpoint on different NIC
target = RDMAEndpoint(device_name=devices[-1], ib_port=1, link_type="Ethernet")

# Create a one-initialized CUDA tensor on GPU 1 as remote buffer
remote_tensor = torch.ones([16], device="cuda:1", dtype=torch.uint8)
# Register target's GPU memory
target.register_memory_region(
    mr_identifier="buffer",
    virtual_address=remote_tensor.data_ptr(),
    length_bytes=remote_tensor.numel() * remote_tensor.itemsize,
)

# Establish bidirectional RDMA connection:
# 1. Target connects to initiator's endpoint information
# 2. Initiator connects to target's endpoint information
# Note: Real-world scenarios typically use out-of-band exchange (e.g., via TCP)
target.connect_to(initiator.local_endpoint_info)
initiator.connect_to(target.local_endpoint_info)

# Execute asynchronous batch read operation:
# - Read 8 bytes from target's "buffer" at offset 0
# - Write to initiator's "buffer" at offset 0
# - asyncio.run() executes the async operation synchronously for demonstration
asyncio.run(
    initiator.async_read_batch(
        mr_key="buffer",
        target_offset=[0],
        source_offset=[8],  # Write to start of local buffer
        length=8,
    )
)

# Verify data transfer:
# - Local tensor should now contain data from remote tensor's first 8 elements
# - Remote tensor remains unchanged (RDMA read is non-destructive)

assert torch.all(local_tensor[:8] == 0)
assert torch.all(local_tensor[8:] == 1)
print("Local tensor after RDMA read:", local_tensor)

```

## Build

``` bash
# on CentOS
sudo yum install cppzmq-devel gflags-devel  cmake 

# on Ubuntu
sudo apt install libzmq-dev libgflags-dev cmake

# build from source
mkdir build; cd build
cmake -DBUILD_BENCH=ON -DBUILD_PYTHON=ON ..; make
```

## Benchmark

``` bash
# Target
./bench/transfer_bench                \
  --remote-endpoint=10.130.8.138:8000 \
  --local-endpoint=10.130.8.139:8000  \
  --device-name="mlx5_bond_0"         \
  --mode target                       \
  --block-size=2048000                \
  --batch-size=160

# Initiator
./bench/transfer_bench                \
  --remote-endpoint=10.130.8.139:8000 \ 
  --local-endpoint=10.130.8.138:8000  \ 
  --device-name="mlx5_bond_0"         \
  --mode initiator                    \
  --block-size=16384                  \
  --batch-size=16                     \
  --duration 10
```

- Cross node performance in H800 with NIC ("mlx5_bond_0"), RoCE v2.

```
Batch size        : 160
Block size        : 8192
Total trips       : 59391
Total transferred : 74238 MiB
Duration          : 10.0001 seconds
Average Latency   : 0.168377 ms/trip
Throughput        : 7423.8 MiB/s
```

```
Batch size        : 160
Block size        : 16384
Total trips       : 51144
Total transferred : 127860 MiB
Duration          : 10.0002 seconds
Average Latency   : 0.19553 ms/trip
Throughput        : 12785.8 MiB/s
```

```
Batch size        : 160
Block size        : 32768
Total trips       : 36614
Total transferred : 183070 MiB
Duration          : 10.0002 seconds
Average Latency   : 0.273124 ms/trip
Throughput        : 18306.7 MiB/s
```

```
Batch size        : 160
Block size        : 65536
Total trips       : 21021
Total transferred : 210210 MiB
Duration          : 10.0003 seconds
Average Latency   : 0.475729 ms/trip
Throughput        : 21020.4 MiB/s
```

```
Batch size        : 160
Block size        : 128000
Total trips       : 11419
Total transferred : 223027 MiB
Duration          : 10.0006 seconds
Average Latency   : 0.875789 ms/trip
Throughput        : 22301.3 MiB/s
```

```
Batch size        : 160
Block size        : 256000
Total trips       : 5839
Total transferred : 228085 MiB
Duration          : 10.0015 seconds
Average Latency   : 1.71288 ms/trip
Throughput        : 22805.2 MiB/s
```

```
Batch size        : 160
Block size        : 512000
Total trips       : 2956
Total transferred : 230937 MiB
Duration          : 10.001 seconds
Average Latency   : 3.3833 ms/trip
Throughput        : 23091.3 MiB/s
```

```
Batch size        : 160
Block size        : 1024000
Total trips       : 1486
Total transferred : 232187 MiB
Duration          : 10.0006 seconds
Average Latency   : 6.72986 ms/trip
Throughput        : 23217.4 MiB/s
```

```
Batch size        : 160
Block size        : 2048000
Total trips       : 742
Total transferred : 231875 MiB
Duration          : 10.001 seconds
Average Latency   : 13.4784 ms/trip
Throughput        : 23185.2 MiB/s
```