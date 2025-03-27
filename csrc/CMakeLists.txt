option(BUILD_BENCH   "Build transfer engine benchmark"  OFF)
option(BUILD_PYTHON  "Build transfer engine benchmark"  ON)

add_subdirectory(ops)
add_subdirectory(utils)
add_subdirectory(engine)

if (BUILD_PYTHON)
add_subdirectory(python)
endif (BUILD_PYTHON)

if (BUILD_BENCH)
add_subdirectory(bench)
endif (BUILD_BENCH)
