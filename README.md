# DLSlime Transfer Engine

A Peer to Peer RDMA Transfer Engine.

## Usage

### RDMA READ

- Details in [p2p.py](example/p2p.py)

```python
devices = available_nic()
assert devices, "No RDMA devices."

# Initialize RDMA endpoint
initiator = RDMAEndpoint(device_name=devices[0], ib_port=1, link_type="RoCE")
# Register local GPU memory with RDMA subsystem
local_tensor = torch.tensor(...)
initiator.register_memory_region("buffer", local_tensor...)

# Initialize target endpoint on different NIC
target = RDMAEndpoint(device_name=devices[-1], ib_port=1, link_type="RoCE")
# Register target's GPU memory
remote_tensor = torch.tensor(...)
target.register_memory_region("buffer", remote_tensor...)

# Establish bidirectional RDMA connection:
# 1. Target connects to initiator's endpoint information
# 2. Initiator connects to target's endpoint information
# Note: Real-world scenarios typically use out-of-band exchange (e.g., via TCP)
target.connect(initiator.local_endpoint_info)
initiator.connect(target.local_endpoint_info)

# Execute asynchronous batch read operation:
asyncio.run(initiator.async_read_batch("buffer", [0], [8], 8))
```

### SendRecv

- Details in [sendrecv.py](example/sendrecv.py)

#### Sender

```python
# RDMA init and RDMA Connect just like RDMA Read
...

# RDMA Send
ones = torch.ones([16], dtype=torch.uint8)
endpoint.register_memory_region("buffer", ones.data_ptr(), 16)
asyncio.run(endpoint.send_async("buffer", 0, 8))
```

#### Receiver

```python
# RDMA init and RDMA Connect just like RDMA Read
...

# RDMA Recv
zeros = torch.zeros([16], dtype=torch.uint8)
endpoint.register_memory_region("buffer", zeros.data_ptr(), 16)
asyncio.run(endpoint.recv_async("buffer", 8, 8))
```

## Build

```bash
# on CentOS
sudo yum install cppzmq-devel gflags-devel  cmake

# on Ubuntu
sudo apt install libzmq-dev libgflags-dev cmake

# build from source
mkdir build; cd build
cmake -DBUILD_BENCH=ON -DBUILD_PYTHON=ON ..; make
```

## Benchmark

```bash
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

### Cross node performance

### Single Device

- NVIDIA ConnectX-7 HHHL Adapter Card; 200GbE (default mode) / NDR200 IB; Dual-port QSFP112; PCIe 5.0 x16 with x16 PCIe extension option;
- RoCE v2.

| Block Size | Batch Size | Total Trips | Total Transferred (MiB) | Duration (seconds) | Average Latency (ms/trip) | Throughput (MiB/s) |
| ---------- | ---------- | ----------- | ----------------------- | ------------------ | ------------------------- | ------------------ |
| 8192       | 160        | 249920      | 312400                  | 10.0006            | 0.0400154                 | 31238              |
| 16384      | 160        | 153300      | 383250                  | 10.0012            | 0.0652392                 | 38320.5            |
| 32768      | 160        | 85280       | 426400                  | 10.0013            | 0.117276                  | 42634.4            |
| 65536      | 160        | 44680       | 446800                  | 10.0033            | 0.223887                  | 44665.3            |
| 128000     | 160        | 23340       | 455859                  | 10.0023            | 0.428546                  | 45575.6            |
| 128000     | 160        | 23340       | 455859                  | 10.0028            | 0.428571                  | 45573              |
| 256000     | 160        | 11820       | 461718                  | 10.0135            | 0.847166                  | 46109.6            |
| 512000     | 160        | 5940        | 464062                  | 10.002             | 1.68384                   | 46396.8            |
| 1024000    | 160        | 2980        | 465625                  | 10.0049            | 3.35735                   | 46539.6            |
| 2048000    | 160        | 1500        | 468750                  | 10.0555            | 6.70364                   | 46616.5            |

### Aggregated Transfer

- NVIDIA ConnectX-7 HHHL Adapter Card * 8
- RoCE v2
