import time

import json
import requests

import torch

from slime.transport.rdma_endpoint import RDMAContext
from slime.config import RDMAInfo
from slime import _slime_c

id = remote_info = requests.get(
    "http://10.130.8.139:4469/init_link"
).json()["id"]

length = 16000000000

mr_key = "kv"
rdma_ctx = RDMAContext("mlx5_bond_7", 1, "Ethernet")
rdma_ctx.register_mr(mr_key, length)

requests.post(
    "http://10.130.8.139:4469/register_mr", 
    json={
        "id": id,
        "mr_key": mr_key,
        "length": length
    }
)

local_info = rdma_ctx.get_local_info()
remote_info = requests.get(
    "http://10.130.8.139:4469/get_local_info", json={"id": id}
).json()
remote_info = RDMAInfo(**json.loads(remote_info))
rdma_ctx.construct(remote_info)

requests.post(
    "http://10.130.8.139:4469/exchange_info", 
    json={
        "id": id,
        "info": local_info.model_dump_json()
    }
)

psum = requests.post(
    "http://10.130.8.139:4469/rdma_read", 
    json={
        "id":id,
        "mr_key": mr_key,
        "remote_rkey": rdma_ctx._rdma_context_c.get_r_key(mr_key),
        "length": 1024,
        "offset": 0,
        "remote_addr": rdma_ctx.memory_pool[mr_key].data_ptr()
    }
)
print(psum.json())

psum = requests.post(
    "http://10.130.8.139:4469/rdma_read", 
    json={
        "id":id,
        "mr_key": mr_key,
        "remote_rkey": rdma_ctx._rdma_context_c.get_r_key(mr_key),
        "length": 1024 * 1024,
        "offset": 0,
        "remote_addr": rdma_ctx.memory_pool[mr_key].data_ptr()
    }
)
print(psum.json())

psum = requests.post(
    "http://10.130.8.139:4469/rdma_read", 
    json={
        "id":id,
        "mr_key": mr_key,
        "remote_rkey": rdma_ctx._rdma_context_c.get_r_key(mr_key),
        "length": 1024 * 1024 * 1024,
        "offset": 0,
        "remote_addr": rdma_ctx.memory_pool[mr_key].data_ptr()
    }
)
print(psum.json())

psum = requests.get(
    "http://10.130.8.139:4469/stop_link", 
    json={
        "id":id,
    }
)
print(psum.json())

rdma_ctx._rdma_context_c.stop_cq_future()
