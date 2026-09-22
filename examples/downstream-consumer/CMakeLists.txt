# Standalone downstream consumer of the installed Fabric Compatibility Registry
# package. This project is deliberately NOT part of the runtime build: it only
# sees the exported CMake package and the installed headers, which is what
# proves the install/export surface actually works.
#
#   cmake -S examples/downstream-consumer -B <build-dir> ^
#         -DCMAKE_PREFIX_PATH=<install-prefix>
#   cmake --build <build-dir>
#
cmake_minimum_required(VERSION 3.25)
project(FcrDownstreamConsumer LANGUAGES CXX)

find_package(FabricCompatibilityRegistry 1.0 REQUIRED)

add_executable(consumer main.cpp)
target_compile_features(consumer PRIVATE cxx_std_20)
target_link_libraries(consumer PRIVATE Fabric::fcr)
set_target_properties(consumer PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
  target_compile_options(consumer PRIVATE /W4 /WX /permissive- /utf-8 /EHsc /Zc:__cplusplus)
endif()
