cmake_minimum_required(VERSION 3.10)
project(hdf5_compress_test)

# use C++11
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# find HDF5 (require C and C++ API)
find_package(HDF5 REQUIRED COMPONENTS C CXX)

message(STATUS "HDF5 found:")
message(STATUS "  Includes: ${HDF5_INCLUDE_DIRS}")
message(STATUS "  Libs:     ${HDF5_LIBRARIES}")
message(STATUS "  CXX Libs: ${HDF5_CXX_LIBRARIES}")

include_directories(${HDF5_INCLUDE_DIRS})

# source files
file(GLOB SRC_FILES
    "src/src_code.cpp"
)

add_executable(hdf5_compress_test ${SRC_FILES})

# link HDF5
target_link_libraries(hdf5_compress_test
    ${HDF5_LIBRARIES}
    ${HDF5_CXX_LIBRARIES}
)

