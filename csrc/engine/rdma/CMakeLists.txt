add_library(
    _slime_rdma
    SHARED
    memory_pool.cpp
    rdma_assignment.cpp
    rdma_transport.cpp
    rdma_scheduler.cpp
)

target_link_libraries(_slime_rdma PUBLIC _slime_engine _slime_utils ibverbs zmq)
