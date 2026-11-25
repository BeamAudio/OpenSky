cmake_minimum_required(VERSION 3.18)
project(JSBSim VERSION 1.3.4 LANGUAGES C CXX)

# Build the upstream JSBSim source as a separate LGPL shared library. The
# simulator links to this target at runtime; its sources are never compiled
# into the OpenSky executable.
set(BUILD_SHARED_LIBS ON CACHE BOOL "Build JSBSim as a shared library" FORCE)
set(BUILD_DOCS OFF CACHE BOOL "Do not build JSBSim documentation" FORCE)
set(BUILD_PYTHON_MODULE OFF CACHE BOOL "Do not build the JSBSim Python module" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(PROJECT_VERSION_MAJOR "1")
set(PROJECT_VERSION_MINOR "3")
set(PROJECT_VERSION_PATCH "4")
set(LIBRARY_VERSION "1.3.4")
set(LIBRARY_SOVERSION "1")

add_subdirectory(src)
