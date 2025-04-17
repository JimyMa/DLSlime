add_executable(
    transfer_bench
    transfer_bench.cpp
)

target_include_directories(transfer_bench PUBLIC ${ZeroMQ_INCLUDE_DIRS})

target_link_libraries(
    transfer_bench
    PUBLIC
    _slime_rdma gflags zmq
)

add_executable(
    transfer_scheduler
    transfer_scheduler.cpp
)

target_include_directories(transfer_scheduler PUBLIC ${ZeroMQ_INCLUDE_DIRS})

target_link_libraries(
    transfer_scheduler
    PUBLIC
    _slime_rdma gflags zmq
)