add_executable(topology_core_test topology_core_test.cpp)
target_link_libraries(topology_core_test PRIVATE DLSlime::topo_core)
add_test(NAME topology_core_test COMMAND topology_core_test)

if(BUILD_TOPO_CUDA)
    add_executable(cuda_topology_backend_test cuda_topology_backend_test.cpp)
    target_link_libraries(cuda_topology_backend_test PRIVATE dlslime::topo_cuda)
    add_test(NAME cuda_topology_backend_test COMMAND cuda_topology_backend_test)
endif()
