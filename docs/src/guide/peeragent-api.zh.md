# PeerAgent API

PeerAgent 是 DLSlime 面向服务场景的 endpoint facade。它会向 NanoCtrl 注册 agent 和内存元数据，
通过 Redis 交换连接信息，并提供命名 I/O helper，应用代码不需要手动交换 RDMA endpoint metadata。

适合使用 PeerAgent 的场景包括：动态 peer 发现、多租户或多任务 `scope` 隔离、命名 memory region、
以及需要自动清理过期控制面状态的服务。

## 启动 Agent

```python
from dlslime import start_peer_agent

agent = start_peer_agent(
    ctrl_url="http://127.0.0.1:4479",
    alias="worker-a",   # 可选；也可由 NanoCtrl 自动分配
    device="mlx5_0",    # 可选首选 NIC
    scope="job-123",    # 可选隔离前缀
)
```

也可以使用 context manager 自动清理：

```python
with start_peer_agent(scope="example") as agent:
    print(agent.alias)
```

## 发现与连接

```python
peers = agent.list_agents()
resource = agent.get_resource("worker-b")
memory_keys = agent.list_mem_keys("worker-b")

conn = agent.connect_to("worker-b", ib_port=1, qp_num=1)
conn.wait(timeout=60)
```

### 选择传输层

兼容旧行为时可继续使用 `transport="rdma"`。跨节点 MNNVL CUDA Fabric
连接可显式使用 `transport="nvlink"`。部署代码也可使用 `transport="auto"`：
MNNVL membership 匹配且存在共同 IMEX channel 时选择 NVLink，否则选择兼容
RDMA，再不可用则失败。`auto` 不会静默回退 TCP，TCP 必须显式请求。

```python
region = agent.allocate_memory_region("kv", 64 * 1024 * 1024)
conn = agent.connect_to("worker-b", transport="nvlink")
conn.sync_memory_regions(timeout=60)
conn.wait(timeout=60)
assert conn.transport == "nvlink"
assert conn.selection_reason == "explicit_request"

auto_conn = agent.connect_to("worker-c", transport="auto")
assert auto_conn.selection_reason == "matching_mnnvl_fabric"
```

`local_device` 和 `peer_device` 可使用进程可见 CUDA ordinal、GPU UUID 或
`cuda:GPU-...`。每个 Ray actor 只看到一张 MNNVL GPU 时无需显式传入。

`dlslime-ctrl status` 只用于确认控制面健康。本地 CUDA facts 使用
`discover_cuda_topology.py --json` 检查；双节点示例还会打印两端注册的
PeerAgent resource，用于确认 MNNVL membership 匹配且存在共同可读的 IMEX channel。

常用方法：

| 方法                             | 作用                                          |
| -------------------------------- | --------------------------------------------- |
| `list_agents()`                  | 返回当前 `scope` 下可见的 active aliases。    |
| `get_resource(peer_alias=None)`  | 返回本地或 peer 的 topology/resource 元数据。 |
| `list_mem_keys(peer_alias=None)` | 返回本地或 peer 已注册的 memory-region 名称。 |
| `connect_to(peer_alias, ...)`    | 建立到指定 peer 的 directed connection。      |
| `get_connections()`              | 返回按 peer alias 分组的连接句柄。            |

所有需要互相发现的 agent 必须使用同一个 `scope`。

## 内存区域

```python
handler = agent.register_memory_region(
    "kv",
    tensor.data_ptr(),
    int(tensor.storage_offset()),
    tensor.numel() * tensor.itemsize,
)

remote_handle = agent.get_handle("kv", peer_alias="worker-b")
peer_info = agent.get_mr_info("kv", peer_alias="worker-b")
```

底层 tensor 或 buffer 必须在 memory region 注册期间保持存活，并且所有相关 future 完成前不能释放或复用。

PeerAgent 也可以直接分配并持有可导出的 CUDA Fabric VMM memory：

```python
region = agent.allocate_memory_region(
    "kv_cache", 256 * 1024 * 1024, memory_kind="cuda_fabric"
)
print(region.ptr, region.length, region.device)
```

应用可将 `region.ptr` 零拷贝包装为 CUDA tensor。该 allocation 的生命周期
由 PeerAgent 管理；两台主机还必须有共同可读的 IMEX channel，才能完成
Fabric handle export/import。

## 命名 I/O

PeerAgent 支持两种 assignment：

- 命名形式：`("mr_name", target_offset, source_offset, length)`
- handle 形式：`(local_handle, remote_handle, target_offset, source_offset, length)`

命名形式更适合普通服务代码：

```python
future = agent.read("worker-b", [("kv", 8, 0, 8)])
future.wait()
```

可用 I/O 方法：

| 方法                                                               | 作用                                  |
| ------------------------------------------------------------------ | ------------------------------------- |
| `read(peer_alias, assignments, stream=None)`                       | 通过已选择的 transport 从 peer 读取。 |
| `write(peer_alias, assignments, stream=None)`                      | 通过已选择的 transport 写入 peer。    |
| `write_with_imm(peer_alias, assignments, imm_data=0, stream=None)` | 携带 immediate data 的 RDMA write。   |
| `send(peer_alias, chunk, stream_handler=None)`                     | 消息发送。                            |
| `recv(peer_alias, chunk, stream_handler=None)`                     | 消息接收。                            |
| `imm_recv(peer_alias, stream=None)`                                | 接收 immediate-data event。           |
