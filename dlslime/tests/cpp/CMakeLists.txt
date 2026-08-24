add_subdirectory(topology)

if (BUILD_ASCEND_DIRECT)
    add_subdirectory(ascend_direct)
endif()
