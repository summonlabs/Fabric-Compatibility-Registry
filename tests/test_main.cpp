#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>

namespace fcr::test {

std::vector<TestCase>& Registry() {
  static std::vector<TestCase> registry;
  return registry;
}

void Check(bool condition, const char* expression, const char* file, int line) {
  if (!condition) {
    std::ostringstream stream;
    stream << file << ":" << line << ": check failed: " << expression;
    throw Failure(stream.str());
  }
}

int RunAll(const std::string& filter) {
  std::vector<TestCase>& registry = Registry();
  std::size_t passed = 0;
  std::size_t failed = 0;
  std::vector<std::string> failures;
  const auto start = std::chrono::steady_clock::now();
  for (const TestCase& test : registry) {
    const std::string full = test.suite + "." + test.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) continue;
    // The test name is announced and flushed before the body runs, so a
    // crash or a hang names the responsible test immediately.
    std::cout << "[ run  ] " << full << std::endl;
    try {
      test.body();
      ++passed;
      std::cout << "[ pass ] " << full << "\n";
    } catch (const Failure& failure) {
      ++failed;
      failures.push_back(full + ": " + failure.message);
      std::cout << "[ FAIL ] " << full << ": " << failure.message << "\n";
    } catch (const std::exception& error) {
      ++failed;
      failures.push_back(full + ": unexpected exception: " + error.what());
      std::cout << "[ FAIL ] " << full << ": unexpected exception: " << error.what() << "\n";
    } catch (...) {
      ++failed;
      failures.push_back(full + ": unexpected non-standard exception");
      std::cout << "[ FAIL ] " << full << ": unexpected non-standard exception\n";
    }
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  std::cout << "\n" << passed << " passed, " << failed << " failed (" << elapsed.count()
            << " ms)\n";
  if (!failures.empty()) {
    std::cout << "\nfailures:\n";
    for (const std::string& failure : failures) std::cout << "  " << failure << "\n";
  }
  return failed == 0 ? 0 : 1;
}

}  // namespace fcr::test

int main(int argc, char** argv) {
  std::string filter;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
      filter = argv[++i];
    }
  }
  return fcr::test::RunAll(filter);
}
