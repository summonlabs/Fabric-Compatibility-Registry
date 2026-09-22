// Fabric Compatibility Registry - Summon Software Labs
// Independent operating-system process helper for the process-level tests.
// These tests prove behaviour across real process boundaries, not threads.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "fcr/fcr.hpp"

namespace fcr::test {

struct ProcessResult {
  int exit_code = -1;
  std::string standard_output;
  std::string standard_error;
};

// Owns a child process created with CreateProcessW.
class ChildProcess {
 public:
  static Result<std::unique_ptr<ChildProcess>> Spawn(const std::string& executable,
                                                     const std::vector<std::string>& arguments,
                                                     const std::string& working_directory,
                                                     bool capture_stdout);

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ~ChildProcess();

  bool Running() const;
  // Reads one line from the captured standard output. Blocks until a newline
  // arrives, so a readiness handshake needs no polling or timeout.
  Result<std::string> ReadLine();
  // Terminates the process immediately (Win32 TerminateProcess) and reaps it.
  ProcessResult Kill();
  // Waits for natural termination and collects captured output.
  ProcessResult Wait();

 private:
  ChildProcess() = default;

  void* process_ = nullptr;
  void* thread_ = nullptr;
  void* stdout_read_ = nullptr;
  bool captured_ = false;
  bool reaped_ = false;
  std::string pending_;
  bool eof_ = false;
};

ProcessResult RunProcess(const std::string& executable, const std::vector<std::string>& arguments,
                         const std::string& working_directory);

Result<std::string> ReadTextFile(const std::string& path);
Status WriteTextFile(const std::string& path, const std::string& text);
bool FileExists(const std::string& path);

// Sleeps for the requested number of milliseconds. Only used to give a child
// process time to reach a well-defined state; no test is bounded by a timeout.
void SleepMilliseconds(unsigned milliseconds);

}  // namespace fcr::test
