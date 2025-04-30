add_subdirectory(utils)
add_subdirectory(engine)

if (BUILD_PYTHON)
add_subdirectory(python)

install(
    TARGETS
    _slime_engine _slime_rdma _slime_utils _slime_c
    LIBRARY DESTINATION "${SKBUILD_PROJECT_NAME}/"
)

set_target_properties(
    _slime_engine _slime_rdma _slime_c _slime_utils
    PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "\${ORIGIN}"
)

endif (BUILD_PYTHON)
