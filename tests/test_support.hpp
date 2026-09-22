// Fabric Compatibility Registry - Summon Software Labs
// Minimal deterministic test harness. No timeouts: a hanging test is a defect
// to diagnose, not something to hide behind a watchdog.
#pragma once

#include <cstdint>
#include <exception>

#include "fcr/error.hpp"
#include <functional>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace fcr::test {

struct Failure : std::exception {
  std::string message;
  explicit Failure(std::string text) : message(std::move(text)) {}
  const char* what() const noexcept override { return message.c_str(); }
};

struct TestCase {
  std::string suite;
  std::string name;
  std::function<void()> body;
};

std::vector<TestCase>& Registry();
int RunAll(const std::string& filter);

struct Registrar {
  Registrar(const char* suite, const char* name, std::function<void()> body) {
    Registry().push_back(TestCase{suite, name, std::move(body)});
  }
};

template <class T>
std::string Describe(const T& value) {
  std::ostringstream stream;
  if constexpr (std::is_same_v<T, bool>) {
    stream << (value ? "true" : "false");
  } else if constexpr (std::is_enum_v<T>) {
    stream << static_cast<long long>(value);
  } else if constexpr (std::is_arithmetic_v<T>) {
    stream << value;
  } else if constexpr (std::is_convertible_v<T, std::string>) {
    stream << std::string(value);
  } else {
    stream << "<value>";
  }
  return stream.str();
}

// Success predicates that work for both Result<T> and Status.
template <class T>
bool Succeeded(const Result<T>& result) {
  return result.has_value();
}
inline bool Succeeded(const Status& status) { return status.ok(); }

template <class T>
std::string FailureText(const Result<T>& result) {
  return result.error().ToString();
}
inline std::string FailureText(const Status& status) { return status.error().ToString(); }

void Check(bool condition, const char* expression, const char* file, int line);

template <class A, class B>
void CheckEq(const A& left, const B& right, const char* expression, const char* file, int line) {
  if (!(left == right)) {
    std::ostringstream stream;
    stream << file << ":" << line << ": check failed: " << expression << " (left="
           << Describe(left) << ", right=" << Describe(right) << ")";
    throw Failure(stream.str());
  }
}

}  // namespace fcr::test

#define FCR_TEST(suite_name, test_name)                                              \
  static void fcr_test_##suite_name##_##test_name();                                 \
  static const ::fcr::test::Registrar fcr_registrar_##suite_name##_##test_name(      \
      #suite_name, #test_name, fcr_test_##suite_name##_##test_name);                 \
  static void fcr_test_##suite_name##_##test_name()

#define CHECK(condition) \
  ::fcr::test::Check((condition), #condition, __FILE__, __LINE__)

#define CHECK_EQ(left, right) \
  ::fcr::test::CheckEq((left), (right), #left " == " #right, __FILE__, __LINE__)

#define CHECK_FALSE(condition) \
  ::fcr::test::Check(!(condition), "!(" #condition ")", __FILE__, __LINE__)

#define FCR_FAIL(message)                                        \
  do {                                                           \
    std::ostringstream fcr_stream;                               \
    fcr_stream << __FILE__ << ":" << __LINE__ << ": " << message; \
    throw ::fcr::test::Failure(fcr_stream.str());                \
  } while (false)

// Validation-report helpers render the full report on failure, so a broken
// expectation is diagnosable from the test log alone.
#define CHECK_PUBLISHABLE(report)                                                     \
  do {                                                                                \
    auto&& fcr_report = (report);                                                     \
    if (!fcr_report.Publishable()) {                                                  \
      FCR_FAIL("expected a publishable report but got:\n" << fcr_report.Render());   \
    }                                                                                 \
  } while (false)

#define CHECK_NOT_PUBLISHABLE(report)                                                 \
  do {                                                                                \
    auto&& fcr_report = (report);                                                     \
    if (fcr_report.Publishable()) {                                                   \
      FCR_FAIL("expected validation errors but the report is publishable");           \
    }                                                                                 \
  } while (false)

// Result helpers keep the failure message close to the assertion.
#define CHECK_OK(expression)                                                          \
  do {                                                                                \
    auto&& fcr_result = (expression);                                                 \
    if (!::fcr::test::Succeeded(fcr_result)) {                                        \
      FCR_FAIL(#expression " failed: " << ::fcr::test::FailureText(fcr_result));      \
    }                                                                                 \
  } while (false)

#define CHECK_ERR(expression, expected_code)                                          \
  do {                                                                                \
    auto&& fcr_result = (expression);                                                 \
    if (fcr_result.has_value()) {                                                     \
      FCR_FAIL(#expression " unexpectedly succeeded");                                \
    }                                                                                 \
    if (fcr_result.error().code != (expected_code)) {                                 \
      FCR_FAIL(#expression " produced " << fcr_result.error().ToString()              \
                                        << " instead of " << #expected_code);         \
    }                                                                                 \
  } while (false)
