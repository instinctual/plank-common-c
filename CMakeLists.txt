cmake_minimum_required(VERSION 3.16)
project(plank_host_protocol LANGUAGES NONE)

# Host-only protocol declarations; no legacy transport library is built.
add_library(plank_host_protocol INTERFACE)
target_include_directories(plank_host_protocol INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_sources(plank_host_protocol INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Limelight.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/plank.h")
