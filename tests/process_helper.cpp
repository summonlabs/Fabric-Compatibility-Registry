#include "process_helper.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fcr::test {
namespace {

std::wstring Widen(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                       nullptr, 0);
  std::wstring out(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
  return out;
}

std::wstring QuoteArgument(const std::string& argument) {
  const std::wstring wide = Widen(argument);
  if (wide.find(L' ') == std::wstring::npos && wide.find(L'\t') == std::wstring::npos) {
    return wide;
  }
  std::wstring out = L"\"";
  for (wchar_t c : wide) {
    if (c == L'"') out.push_back(L'\\');
    out.push_back(c);
  }
  out.push_back(L'"');
  return out;
}

std::string ReadAll(void* handle) {
  std::string out;
  char buffer[4096];
  DWORD read = 0;
  while (ReadFile(static_cast<HANDLE>(handle), buffer, sizeof(buffer), &read, nullptr) &&
         read != 0) {
    out.append(buffer, read);
  }
  // A captured stdout is a pipe, but the C runtime still translates line ends
  // to CRLF in text mode. Normalise so assertions compare logical lines.
  std::string normalised;
  normalised.reserve(out.size());
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (out[i] == '\r' && i + 1 < out.size() && out[i + 1] == '\n') continue;
    normalised.push_back(out[i]);
  }
  return normalised;
}

}  // namespace

void SleepMilliseconds(unsigned milliseconds) {
  std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

Result<std::unique_ptr<ChildProcess>> ChildProcess::Spawn(const std::string& executable,
                                                          const std::vector<std::string>& arguments,
                                                          const std::string& working_directory,
                                                          bool capture_stdout) {
  auto child = std::unique_ptr<ChildProcess>(new ChildProcess());
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;

  HANDLE stdout_read = nullptr;
  HANDLE stdout_write = nullptr;
  if (capture_stdout) {
    if (!CreatePipe(&stdout_read, &stdout_write, &attributes, 0)) {
      return MakeError(ErrorCode::IoError, "cannot create the child stdout pipe");
    }
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
  } else {
    stdout_write = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &attributes, OPEN_EXISTING, 0, nullptr);
  }

  std::wstring command = QuoteArgument(executable);
  for (const std::string& argument : arguments) {
    command.push_back(L' ');
    command.append(QuoteArgument(argument));
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = stdout_write;
  startup.hStdError = stdout_write;
  startup.hStdInput = nullptr;

  PROCESS_INFORMATION info{};
  std::wstring mutable_command = command;
  const std::wstring working = Widen(working_directory);
  const BOOL created =
      CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                     nullptr, working.empty() ? nullptr : working.c_str(), &startup, &info);
  if (stdout_write != nullptr) CloseHandle(static_cast<HANDLE>(stdout_write));
  if (!created) {
    if (stdout_read != nullptr) CloseHandle(stdout_read);
    return MakeError(ErrorCode::IoError, "cannot start child process",
                     executable + " (error " + std::to_string(GetLastError()) + ")");
  }
  child->process_ = info.hProcess;
  child->thread_ = info.hThread;
  child->stdout_read_ = stdout_read;
  child->captured_ = capture_stdout;
  return child;
}

ChildProcess::~ChildProcess() {
  if (!reaped_ && process_ != nullptr) {
    TerminateProcess(static_cast<HANDLE>(process_), 1);
    WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
  }
  if (stdout_read_ != nullptr) CloseHandle(static_cast<HANDLE>(stdout_read_));
  if (thread_ != nullptr) CloseHandle(static_cast<HANDLE>(thread_));
  if (process_ != nullptr) CloseHandle(static_cast<HANDLE>(process_));
}

bool ChildProcess::Running() const {
  if (process_ == nullptr) return false;
  DWORD code = 0;
  if (!GetExitCodeProcess(static_cast<HANDLE>(process_), &code)) return false;
  return code == STILL_ACTIVE;
}

Result<std::string> ChildProcess::ReadLine() {
  if (!captured_ || stdout_read_ == nullptr) {
    return MakeError(ErrorCode::InvalidArgument, "child standard output is not captured");
  }
  while (true) {
    const std::size_t newline = pending_.find('\n');
    if (newline != std::string::npos) {
      std::string line = pending_.substr(0, newline);
      pending_.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      return line;
    }
    if (eof_) {
      return MakeError(ErrorCode::NotFound, "child produced no further output");
    }
    char buffer[1024];
    DWORD read = 0;
    if (!ReadFile(static_cast<HANDLE>(stdout_read_), buffer, sizeof(buffer), &read, nullptr) ||
        read == 0) {
      eof_ = true;
      if (pending_.empty()) {
        return MakeError(ErrorCode::NotFound, "child produced no further output");
      }
      std::string line = pending_;
      pending_.clear();
      return line;
    }
    pending_.append(buffer, read);
  }
}

Result<std::string> ReadTextFile(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return MakeError(ErrorCode::NotFound, "cannot open file", path);
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

Status WriteTextFile(const std::string& path, const std::string& text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return Fail(ErrorCode::IoError, "cannot write file '" + path + "'");
  }
  stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!stream) {
    return Fail(ErrorCode::IoError, "cannot write file '" + path + "'");
  }
  return Status{};
}

bool FileExists(const std::string& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

ProcessResult ChildProcess::Kill() {
  ProcessResult result;
  if (process_ == nullptr) return result;
  TerminateProcess(static_cast<HANDLE>(process_), 137);
  WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
  result.exit_code = static_cast<int>(code);
  if (captured_ && stdout_read_ != nullptr) {
    result.standard_output = pending_ + ReadAll(stdout_read_);
    pending_.clear();
  }
  reaped_ = true;
  return result;
}

ProcessResult ChildProcess::Wait() {
  ProcessResult result;
  if (process_ == nullptr) return result;
  // The captured pipe is drained first. A child that writes more than the pipe
  // buffer holds would otherwise block in write while this thread waits for it
  // to exit, which is a deadlock rather than a slow test.
  if (captured_ && stdout_read_ != nullptr) {
    result.standard_output = pending_ + ReadAll(stdout_read_);
    pending_.clear();
  }
  WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
  result.exit_code = static_cast<int>(code);
  reaped_ = true;
  return result;
}

ProcessResult RunProcess(const std::string& executable, const std::vector<std::string>& arguments,
                         const std::string& working_directory) {
  ProcessResult failure;
  failure.exit_code = -1;
  auto child = ChildProcess::Spawn(executable, arguments, working_directory, true);
  if (!child.has_value()) {
    failure.standard_error = child.error().ToString();
    return failure;
  }
  return child.value()->Wait();
}

}  // namespace fcr::test
