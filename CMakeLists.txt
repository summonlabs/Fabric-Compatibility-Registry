cmake_minimum_required(VERSION 3.25)

project(FabricCompatibilityRegistry
        VERSION 1.0.0
        DESCRIPTION "Vendor-neutral compatibility registry runtime for Summon Software Labs Fabric OS"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(FCR_BUILD_TOOLS "Build fcrctl and fcr-registryd" ON)
option(FCR_BUILD_TESTS "Build the validation suite" ON)
option(FCR_BUILD_EXAMPLES "Build the embedding examples" ON)
option(FCR_BUILD_BENCHMARKS "Build the benchmark tool" ON)
option(FCR_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)

if(NOT MSVC)
  message(FATAL_ERROR
          "Fabric Compatibility Registry v1.0.0 targets Windows x64 with MSVC. "
          "The persistence layer uses Win32 file locking and write-through renames, "
          "and the transport layer uses Winsock; neither has been validated elsewhere.")
endif()

set(FCR_WARNING_FLAGS /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus /MP)
if(FCR_WARNINGS_AS_ERRORS)
  list(APPEND FCR_WARNING_FLAGS /WX)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(fcr_core STATIC
  src/authority.cpp
  src/client.cpp
  src/component.cpp
  src/decision_io.cpp
  src/digest.cpp
  src/document.cpp
  src/error.cpp
  src/identity.cpp
  src/json.cpp
  src/protocol.cpp
  src/registry.cpp
  src/rule.cpp
  src/server.cpp
  src/store.cpp
  src/taxonomy.cpp
  src/transport.cpp
  src/validation.cpp
  src/version.cpp
)
add_library(Fabric::fcr ALIAS fcr_core)

target_include_directories(fcr_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(fcr_core PUBLIC cxx_std_20)
target_compile_options(fcr_core PRIVATE ${FCR_WARNING_FLAGS} /Brepro)
target_compile_definitions(fcr_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
# Consumers of the static library need the Winsock import library and the same
# platform definitions, so both are usage requirements.
target_compile_definitions(fcr_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(fcr_core PUBLIC ws2_32)

set_target_properties(fcr_core PROPERTIES
  OUTPUT_NAME "fcr"
  # The exported target is Fabric::fcr, matching the in-tree alias. Without
  # EXPORT_NAME the exported name would follow the internal target name.
  EXPORT_NAME "fcr"
  DEBUG_POSTFIX "d"
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
if(FCR_BUILD_TOOLS)
  add_executable(fcrctl apps/fcrctl/main.cpp apps/cli_common.cpp)
  target_include_directories(fcrctl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps)
  target_link_libraries(fcrctl PRIVATE fcr_core)
  target_compile_options(fcrctl PRIVATE ${FCR_WARNING_FLAGS} /Brepro)
  set_target_properties(fcrctl PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

  add_executable(fcr-registryd apps/registryd/main.cpp)
  target_link_libraries(fcr-registryd PRIVATE fcr_core)
  target_compile_options(fcr-registryd PRIVATE ${FCR_WARNING_FLAGS} /Brepro)
  set_target_properties(fcr-registryd PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---------------------------------------------------------------------------
# Examples, benchmarks, tests
# ---------------------------------------------------------------------------
if(FCR_BUILD_EXAMPLES)
  add_executable(fcr-example-upgrade-manager examples/embed_upgrade_manager.cpp)
  target_link_libraries(fcr-example-upgrade-manager PRIVATE fcr_core)
  target_compile_options(fcr-example-upgrade-manager PRIVATE ${FCR_WARNING_FLAGS} /Brepro)
  set_target_properties(fcr-example-upgrade-manager PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(FCR_BUILD_BENCHMARKS)
  add_executable(fcr-bench apps/fcr-bench/main.cpp)
  target_link_libraries(fcr-bench PRIVATE fcr_core)
  target_compile_options(fcr-bench PRIVATE ${FCR_WARNING_FLAGS} /Brepro)
  set_target_properties(fcr-bench PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(FCR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS fcr_core
        EXPORT FabricCompatibilityRegistryTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(FCR_BUILD_TOOLS)
  install(TARGETS fcrctl fcr-registryd
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY include/fcr DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT FabricCompatibilityRegistryTargets
        FILE FabricCompatibilityRegistryTargets.cmake
        NAMESPACE Fabric::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FabricCompatibilityRegistry)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FabricCompatibilityRegistryConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/FabricCompatibilityRegistryConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FabricCompatibilityRegistry)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/FabricCompatibilityRegistryConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/FabricCompatibilityRegistryConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/FabricCompatibilityRegistryConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FabricCompatibilityRegistry)
