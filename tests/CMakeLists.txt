add_executable(fcr_tests
  test_main.cpp
  fixtures.cpp
  process_helper.cpp
  test_version.cpp
  test_json.cpp
  test_digest.cpp
  test_validation.cpp
  test_engine.cpp
  test_determinism.cpp
  test_store.cpp
  test_fuzz.cpp
  test_concurrency.cpp
  test_state_machine.cpp
  test_transport.cpp
  test_process.cpp
  test_e2e.cpp
  test_hardening.cpp
)
target_link_libraries(fcr_tests PRIVATE fcr_core)
target_compile_options(fcr_tests PRIVATE ${FCR_WARNING_FLAGS} /Brepro)
target_include_directories(fcr_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(fcr_tests PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The process-level tests drive real independent operating-system processes.
target_compile_definitions(fcr_tests PRIVATE
  FCR_REGISTRYD_PATH="$<TARGET_FILE:fcr-registryd>"
  FCR_FCRCTL_PATH="$<TARGET_FILE:fcrctl>")

add_test(NAME fcr_tests COMMAND fcr_tests)
