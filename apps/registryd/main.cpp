// Fabric Compatibility Registry - Summon Software Labs
// fcr-registryd: the registry authority service.
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "fcr/fcr.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

std::mutex g_stop_mutex;
std::condition_variable g_stop_cv;
bool g_stop_requested = false;

void RequestStop() {
  {
    std::lock_guard<std::mutex> lock(g_stop_mutex);
    g_stop_requested = true;
  }
  g_stop_cv.notify_all();
}

void WaitForStop() {
  std::unique_lock<std::mutex> lock(g_stop_mutex);
  g_stop_cv.wait(lock, [] { return g_stop_requested; });
}

#ifdef _WIN32
BOOL WINAPI ConsoleHandler(DWORD signal) {
  if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
    RequestStop();
    return TRUE;
  }
  return FALSE;
}
#endif

struct Args {
  std::filesystem::path data_dir;
  std::string listen = "127.0.0.1:0";
  std::filesystem::path port_file;
  std::size_t workers = 4;
  std::size_t max_connections = 32;
  std::size_t pending_connections = 256;
  std::uint32_t max_frame = fcr::kDefaultMaxFrameBytes;
  bool read_only = false;
  bool allow_shutdown_op = true;
  bool allow_prune = true;
};

void PrintUsage() {
  std::cout <<
      "fcr-registryd - Fabric Compatibility Registry authority service\n"
      "\n"
      "usage: fcr-registryd [options]\n"
      "\n"
      "  --data-dir <dir>          registry data directory (required unless --read-only)\n"
      "  --listen <host:port>      bind address (default 127.0.0.1:0, ephemeral port)\n"
      "  --port-file <file>        write the bound port number to a file\n"
      "  --workers <n>             worker threads (default 4, maximum 64)\n"
      "  --max-connections <n>     concurrent connection limit (default 32)\n"
      "  --pending-connections <n> bounded accept queue (default 256)\n"
      "  --max-frame <bytes>       maximum frame payload (default 1048576)\n"
      "  --read-only               serve an existing directory without write authority\n"
      "  --no-shutdown-op          refuse remote shutdown requests\n"
      "  --no-prune                refuse remote pruning requests\n"
      "\n"
      "Prints 'READY <host>:<port>' on stdout once the listener is accepting.\n";
}

bool ParseSize(const std::string& text, std::size_t* out) {
  if (text.empty()) return false;
  std::size_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') return false;
    value = value * 10u + static_cast<std::size_t>(c - '0');
    if (value > 100000000u) return false;
  }
  *out = value;
  return true;
}

fcr::Result<Args> Parse(int argc, char** argv) {
  Args args;
  args.data_dir = std::filesystem::path("fcr-data");
  {
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, "FCR_DATA_DIR") == 0 && buffer != nullptr) {
      const std::string value(buffer);
      free(buffer);
      if (!value.empty()) args.data_dir = std::filesystem::path(value);
    }
  }
  for (int i = 1; i < argc; ++i) {
    const std::string token = argv[i];
    const auto next = [&](std::string* out) -> bool {
      if (i + 1 >= argc) return false;
      *out = argv[++i];
      return true;
    };
    std::string value;
    if (token == "--help" || token == "-h") {
      PrintUsage();
      std::exit(0);
    } else if (token == "--data-dir") {
      if (!next(&value)) return fcr::MakeError(fcr::ErrorCode::InvalidArgument, "--data-dir needs a value");
      args.data_dir = value;
    } else if (token == "--listen") {
      if (!next(&value)) return fcr::MakeError(fcr::ErrorCode::InvalidArgument, "--listen needs a value");
      args.listen = value;
    } else if (token == "--port-file") {
      if (!next(&value)) return fcr::MakeError(fcr::ErrorCode::InvalidArgument, "--port-file needs a value");
      args.port_file = value;
    } else if (token == "--workers") {
      if (!next(&value) || !ParseSize(value, &args.workers)) {
        return fcr::MakeError(fcr::ErrorCode::InvalidArgument, "--workers needs a number");
      }
    } else if (token == "--max-connections") {
      if (!next(&value) || !ParseSize(value, &args.max_connections)) {
        return fcr::MakeError(fcr::ErrorCode::InvalidArgument, "--max-connections needs a number");
      }
    } else if (token == "--pending-connections") {
      if (!next(&value) || !ParseSize(value, &args.pending_connections)) {
        return fcr::MakeError(fcr::ErrorCode::InvalidArgument,
                              "--pending-connections needs a number");
      }
    } else if (token == "--max-frame") {
      std::size_t frame = 0;
      if (!next(&value) || !ParseSize(value, &frame)) {
        return fcr::MakeError(fcr::ErrorCode::InvalidArgument, "--max-frame needs a number");
      }
      args.max_frame = static_cast<std::uint32_t>(frame);
    } else if (token == "--read-only") {
      args.read_only = true;
    } else if (token == "--no-shutdown-op") {
      args.allow_shutdown_op = false;
    } else if (token == "--no-prune") {
      args.allow_prune = false;
    } else {
      return fcr::MakeError(fcr::ErrorCode::InvalidArgument, "unknown option", token);
    }
  }
  return args;
}

}  // namespace

int main(int argc, char** argv) {
  auto parsed = Parse(argc, argv);
  if (!parsed.has_value()) {
    std::cerr << "error: " << parsed.error().ToString() << "\n";
    PrintUsage();
    return 1;
  }
  const Args args = parsed.value();

  std::string host = args.listen;
  std::uint16_t port = 0;
  {
    const std::size_t colon = args.listen.rfind(':');
    if (colon == std::string::npos) {
      std::cerr << "error: --listen must be host:port\n";
      return 1;
    }
    host = args.listen.substr(0, colon);
    const std::string port_text = args.listen.substr(colon + 1);
    std::size_t value = 0;
    if (!ParseSize(port_text, &value) || value > 65535) {
      std::cerr << "error: --listen port is out of range\n";
      return 1;
    }
    port = static_cast<std::uint16_t>(value);
  }

  fcr::AuthorityOptions options;
  options.store.directory = args.data_dir;
  options.store.mode = args.read_only ? fcr::OpenMode::ReadOnly : fcr::OpenMode::ReadWrite;
  options.allow_empty = true;

  auto authority = fcr::RegistryAuthority::Open(options);
  if (!authority.has_value()) {
    std::cerr << "error: " << authority.error().ToString() << "\n";
    return 1;
  }

  fcr::ServerOptions server_options;
  server_options.bind_address = host;
  server_options.port = port;
  server_options.worker_threads = args.workers;
  server_options.max_connections = args.max_connections;
  server_options.pending_connections = args.pending_connections;
  server_options.max_frame_bytes = args.max_frame;
  server_options.allow_publish = !args.read_only;
  server_options.allow_prune = args.allow_prune && !args.read_only;
  server_options.allow_shutdown = args.allow_shutdown_op;
  server_options.on_shutdown_request = [] { RequestStop(); };

  auto server = fcr::RegistryServer::Start(authority.value(), server_options);
  if (!server.has_value()) {
    std::cerr << "error: " << server.error().ToString() << "\n";
    return 1;
  }

#ifdef _WIN32
  SetConsoleCtrlHandler(ConsoleHandler, TRUE);
#endif

  if (!args.port_file.empty()) {
    std::ofstream port_stream(args.port_file, std::ios::trunc);
    if (!port_stream) {
      std::cerr << "error: cannot write the port file\n";
      server.value()->Shutdown();
      return 1;
    }
    port_stream << server.value()->port() << "\n";
  }

  std::cout << "READY " << host << ":" << server.value()->port() << "\n";
  std::cout.flush();

  WaitForStop();
  server.value()->Shutdown();

  const fcr::ServerStats stats = server.value()->stats();
  std::cout << "STOPPED accepted=" << stats.accepted_connections
            << " rejected=" << stats.rejected_connections
            << " completed=" << stats.completed_requests << " failed=" << stats.failed_requests
            << " protocol_errors=" << stats.protocol_errors << "\n";
  std::cout.flush();
  return 0;
}
