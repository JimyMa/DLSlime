add_subdirectory(ops)
add_subdirectory(utils)
add_subdirectory(engine)

if (BUILD_PYTHON)
add_subdirectory(python)

install(
    TARGETS
    _slime_ops _slime _slime_c _slime_utils 
    LIBRARY DESTINATION "${SKBUILD_PROJECT_NAME}"
)

set_target_properties(
    _slime_ops _slime _slime_c _slime_utils 
    PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "\${ORIGIN}"
)
endif (BUILD_PYTHON)
