#include "fcr/store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <system_error>

#include "fcr/digest.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fcr {
namespace {

constexpr const char* kMetaFileName = "registry.meta";
constexpr const char* kLockFileName = "registry.lock";
constexpr const char* kGenerationPrefix = "gen-";
constexpr const char* kGenerationSuffix = ".fcr";
constexpr std::size_t kGenerationDigits = 20;

std::string FormatGenerationNumber(GenerationNumber number) {
  char buffer[kGenerationDigits + 1];
  std::snprintf(buffer, sizeof(buffer), "%020llu", static_cast<unsigned long long>(number.value()));
  return std::string(buffer);
}

Result<GenerationNumber> ParseGenerationFileName(const std::string& name) {
  if (name.size() <= std::strlen(kGenerationPrefix) + std::strlen(kGenerationSuffix)) {
    return MakeError(ErrorCode::NotFound, "not a generation file");
  }
  if (name.compare(0, std::strlen(kGenerationPrefix), kGenerationPrefix) != 0) {
    return MakeError(ErrorCode::NotFound, "not a generation file");
  }
  if (name.compare(name.size() - std::strlen(kGenerationSuffix), std::strlen(kGenerationSuffix),
                   kGenerationSuffix) != 0) {
    return MakeError(ErrorCode::NotFound, "not a generation file");
  }
  const std::string digits = name.substr(std::strlen(kGenerationPrefix),
                                         name.size() - std::strlen(kGenerationPrefix) -
                                             std::strlen(kGenerationSuffix));
  if (digits.empty() || digits.size() > kGenerationDigits) {
    return MakeError(ErrorCode::NotFound, "not a generation file");
  }
  std::uint64_t value = 0;
  for (char c : digits) {
    if (c < '0' || c > '9') return MakeError(ErrorCode::NotFound, "not a generation file");
    value = value * 10u + static_cast<std::uint64_t>(c - '0');
  }
  return GenerationNumber(value);
}

Timestamp NowTimestamp() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  return Timestamp::FromNanos(static_cast<std::int64_t>(nanos));
}

void StoreLittleEndian64(std::vector<std::byte>& out, std::size_t offset, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out[offset + static_cast<std::size_t>(i)] =
        static_cast<std::byte>((value >> (8 * i)) & 0xffu);
  }
}

void StoreLittleEndian16(std::vector<std::byte>& out, std::size_t offset, std::uint16_t value) {
  out[offset] = static_cast<std::byte>(value & 0xffu);
  out[offset + 1] = static_cast<std::byte>((value >> 8) & 0xffu);
}

void StoreLittleEndian32(std::vector<std::byte>& out, std::size_t offset, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out[offset + static_cast<std::size_t>(i)] =
        static_cast<std::byte>((value >> (8 * i)) & 0xffu);
  }
}

std::uint64_t LoadLittleEndian64(const std::byte* data) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(data[i])) << (8 * i);
  }
  return value;
}

std::uint16_t LoadLittleEndian16(const std::byte* data) {
  return static_cast<std::uint16_t>(std::to_integer<unsigned>(data[0]) |
                                    (std::to_integer<unsigned>(data[1]) << 8));
}

std::uint32_t LoadLittleEndian32(const std::byte* data) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(std::to_integer<unsigned>(data[i])) << (8 * i);
  }
  return value;
}

JsonValue MetaPayloadJson(const StoreMeta& meta) {
  JsonValue out = JsonValue::Obj();
  out.Set("format", JsonValue::Str(std::string(kStoreMetaFormat)));
  out.Set("format_version", JsonValue::UInt(kStoreMetaFormatVersion));
  out.Set("generation", JsonValue::UInt(meta.generation.value()));
  out.Set("generation_digest", JsonValue::Str(meta.generation_digest.ToHex()));
  out.Set("publisher_epoch", JsonValue::UInt(meta.epoch.value()));
  out.Set("incarnation", JsonValue::UInt(meta.incarnation.value()));
  out.Set("total_publications", JsonValue::UInt(meta.total_publications));
  out.Set("updated_at", JsonValue::Str(meta.updated_at.ToIso8601()));
  out.Set("last_publisher", JsonValue::Str(meta.last_publisher.value()));
  return out;
}

std::mutex& WriterRegistryMutex() {
  static std::mutex mutex;
  return mutex;
}

std::set<std::string>& WriterRegistry() {
  static std::set<std::string> registry;
  return registry;
}

std::string CanonicalDirectoryKey(const std::filesystem::path& directory) {
  std::error_code ec;
  std::filesystem::path canonical = std::filesystem::weakly_canonical(directory, ec);
  if (ec) canonical = directory;
  std::string key = canonical.generic_string();
  // Windows paths compare case-insensitively.
  for (char& c : key) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return key;
}

}  // namespace

bool DirectoryHeldForWrite(const std::string& canonical_directory) {
  std::lock_guard<std::mutex> guard(WriterRegistryMutex());
  return WriterRegistry().count(canonical_directory) != 0;
}

JsonValue StoreMetaToJson(const StoreMeta& meta) {
  JsonValue out = MetaPayloadJson(meta);
  const std::string payload = out.Dump(false);
  out.Set("integrity", JsonValue::Str("sha256:" + Sha256::Hash(payload).ToHex()));
  return out;
}

Result<StoreMeta> ParseStoreMeta(const JsonValue& json) {
  if (!json.IsObject()) {
    return MakeError(ErrorCode::CorruptPersistence, "registry metadata must be a JSON object");
  }
  auto integrity = RequireString(json, "integrity", "registry metadata");
  if (!integrity.has_value()) return integrity.error();
  JsonValue payload = JsonValue::Obj();
  for (std::size_t i = 0; i < json.keys().size(); ++i) {
    if (json.keys()[i] == "integrity") continue;
    payload.Set(json.keys()[i], json.values()[i]);
  }
  const std::string expected = "sha256:" + Sha256::Hash(payload.Dump(false)).ToHex();
  if (expected != integrity.value()) {
    return MakeError(ErrorCode::DigestMismatch,
                     "registry metadata integrity check failed");
  }
  auto format = RequireString(json, "format", "registry metadata");
  if (!format.has_value()) return format.error();
  if (format.value() != kStoreMetaFormat) {
    return MakeError(ErrorCode::UnsupportedFormatVersion, "unexpected registry metadata format",
                     format.value());
  }
  auto format_version = RequireUInt(json, "format_version", "registry metadata");
  if (!format_version.has_value()) return format_version.error();
  if (format_version.value() != kStoreMetaFormatVersion) {
    return MakeError(ErrorCode::UnsupportedFormatVersion,
                     "unsupported registry metadata format version",
                     std::to_string(format_version.value()));
  }
  StoreMeta meta;
  auto generation = RequireUInt(json, "generation", "registry metadata");
  if (!generation.has_value()) return generation.error();
  meta.generation = GenerationNumber(generation.value());
  auto digest_text = RequireString(json, "generation_digest", "registry metadata");
  if (!digest_text.has_value()) return digest_text.error();
  auto digest = ContentDigest::FromHex(digest_text.value());
  if (!digest.has_value()) return digest.error();
  meta.generation_digest = digest.value();
  auto epoch = RequireUInt(json, "publisher_epoch", "registry metadata");
  if (!epoch.has_value()) return epoch.error();
  meta.epoch = PublisherEpoch(epoch.value());
  auto incarnation = RequireUInt(json, "incarnation", "registry metadata");
  if (!incarnation.has_value()) return incarnation.error();
  meta.incarnation = Incarnation(incarnation.value());
  auto publications = RequireUInt(json, "total_publications", "registry metadata");
  if (!publications.has_value()) return publications.error();
  meta.total_publications = publications.value();
  auto updated_at = RequireString(json, "updated_at", "registry metadata");
  if (!updated_at.has_value()) return updated_at.error();
  auto timestamp = Timestamp::FromIso8601(updated_at.value());
  if (!timestamp.has_value()) return timestamp.error();
  meta.updated_at = timestamp.value();
  auto publisher = OptionalString(json, "last_publisher", "");
  if (!publisher.has_value()) return publisher.error();
  if (!publisher.value().empty()) {
    auto id = PublisherId::Parse(publisher.value());
    if (!id.has_value()) return id.error();
    meta.last_publisher = id.value();
  }
  return meta;
}

Result<std::vector<std::byte>> RegistryStore::EncodeGeneration(const RegistryDocument& document,
                                                               GenerationNumber generation,
                                                               PublisherEpoch epoch,
                                                               Incarnation incarnation,
                                                               std::uint64_t max_bytes) {
  RegistryDocument to_encode = document;
  to_encode.meta.generation = generation;
  to_encode.meta.epoch = epoch;
  to_encode.Normalize();
  const std::string payload = to_encode.CanonicalJson();
  if (static_cast<std::uint64_t>(payload.size()) > max_bytes) {
    return MakeError(ErrorCode::LimitExceeded, "registry document exceeds the configured size limit");
  }
  const ContentDigest digest =
      Sha256::Hash(std::span<const std::byte>(reinterpret_cast<const std::byte*>(payload.data()),
                                              payload.size()));
  std::vector<std::byte> out(kGenerationHeaderBytes + payload.size(), std::byte{0});
  out[0] = std::byte{'F'};
  out[1] = std::byte{'C'};
  out[2] = std::byte{'R'};
  out[3] = std::byte{'G'};
  StoreLittleEndian16(out, 4, static_cast<std::uint16_t>(kGenerationFormatVersion));
  StoreLittleEndian16(out, 6, static_cast<std::uint16_t>(kGenerationHeaderBytes));
  StoreLittleEndian64(out, 8, generation.value());
  StoreLittleEndian64(out, 16, epoch.value());
  StoreLittleEndian64(out, 24, incarnation.value());
  StoreLittleEndian64(out, 32, static_cast<std::uint64_t>(payload.size()));
  std::memcpy(out.data() + 40, digest.bytes().data(), ContentDigest::kSize);
  const std::uint32_t header_crc =
      Crc32c(std::span<const std::byte>(out.data(), static_cast<std::size_t>(72)));
  StoreLittleEndian32(out, 72, header_crc);
  std::memcpy(out.data() + kGenerationHeaderBytes, payload.data(), payload.size());
  return out;
}

Result<RegistryDocument> RegistryStore::DecodeGeneration(const std::vector<std::byte>& bytes,
                                                         GenerationNumber* generation,
                                                         PublisherEpoch* epoch,
                                                         Incarnation* incarnation) {
  if (bytes.size() < kGenerationHeaderBytes) {
    return MakeError(ErrorCode::CorruptPersistence, "generation file is shorter than its header");
  }
  const std::byte* header = bytes.data();
  if (std::to_integer<char>(header[0]) != 'F' || std::to_integer<char>(header[1]) != 'C' ||
      std::to_integer<char>(header[2]) != 'R' || std::to_integer<char>(header[3]) != 'G') {
    return MakeError(ErrorCode::CorruptPersistence, "generation file magic is not 'FCRG'");
  }
  const std::uint16_t format_version = LoadLittleEndian16(header + 4);
  if (format_version != kGenerationFormatVersion) {
    return MakeError(ErrorCode::UnsupportedFormatVersion,
                     "unsupported generation file format version",
                     std::to_string(format_version));
  }
  const std::uint16_t header_size = LoadLittleEndian16(header + 6);
  if (header_size != kGenerationHeaderBytes) {
    return MakeError(ErrorCode::CorruptPersistence, "generation file header size is not supported",
                     std::to_string(header_size));
  }
  const std::uint32_t stored_crc = LoadLittleEndian32(header + 72);
  const std::uint32_t actual_crc =
      Crc32c(std::span<const std::byte>(bytes.data(), static_cast<std::size_t>(72)));
  if (stored_crc != actual_crc) {
    return MakeError(ErrorCode::CorruptPersistence, "generation file header checksum mismatch");
  }
  const std::uint64_t payload_length = LoadLittleEndian64(header + 32);
  if (payload_length != bytes.size() - kGenerationHeaderBytes) {
    return MakeError(ErrorCode::CorruptPersistence,
                     "generation file payload length does not match the file size");
  }
  const std::byte* payload = bytes.data() + kGenerationHeaderBytes;
  const ContentDigest stored_digest = ContentDigest::FromBytes([&] {
    std::array<std::byte, ContentDigest::kSize> raw{};
    std::memcpy(raw.data(), header + 40, ContentDigest::kSize);
    return raw;
  }());
  const ContentDigest actual_digest =
      Sha256::Hash(std::span<const std::byte>(payload, static_cast<std::size_t>(payload_length)));
  if (!(stored_digest == actual_digest)) {
    return MakeError(ErrorCode::DigestMismatch, "generation file payload digest mismatch");
  }
  const std::string_view text(reinterpret_cast<const char*>(payload),
                              static_cast<std::size_t>(payload_length));
  auto document = ParseRegistryDocumentText(text);
  if (!document.has_value()) return document.error();
  const GenerationNumber stored_generation(LoadLittleEndian64(header + 8));
  if (!(document.value().meta.generation == stored_generation)) {
    return MakeError(ErrorCode::CorruptPersistence,
                     "generation file header disagrees with the document payload");
  }
  if (!(document.value().Digest() == actual_digest)) {
    return MakeError(ErrorCode::DigestMismatch,
                     "canonical digest of the decoded document does not match the stored digest");
  }
  if (generation != nullptr) *generation = stored_generation;
  if (epoch != nullptr) *epoch = PublisherEpoch(LoadLittleEndian64(header + 16));
  if (incarnation != nullptr) *incarnation = Incarnation(LoadLittleEndian64(header + 24));
  return document;
}

Result<std::vector<std::byte>> RegistryStore::ReadFileBounded(const std::filesystem::path& path,
                                                              std::uint64_t max_bytes) const {
#ifdef _WIN32
  HANDLE handle = CreateFileW(path.wstring().c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return MakeError(ErrorCode::NotFound, "cannot open file for reading", path.string());
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(handle, &size)) {
    CloseHandle(handle);
    return MakeError(ErrorCode::IoError, "cannot determine file size", path.string());
  }
  if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > max_bytes) {
    CloseHandle(handle);
    return MakeError(ErrorCode::LimitExceeded, "file exceeds the configured size limit",
                     path.string());
  }
  std::vector<std::byte> out(static_cast<std::size_t>(size.QuadPart));
  std::size_t offset = 0;
  while (offset < out.size()) {
    const DWORD want = static_cast<DWORD>(
        std::min<std::size_t>(out.size() - offset, 1u << 20));
    DWORD read = 0;
    if (!ReadFile(handle, out.data() + offset, want, &read, nullptr)) {
      CloseHandle(handle);
      return MakeError(ErrorCode::IoError, "read failed", path.string());
    }
    if (read == 0) break;
    offset += read;
  }
  CloseHandle(handle);
  if (offset != out.size()) {
    return MakeError(ErrorCode::CorruptPersistence, "short read", path.string());
  }
  return out;
#else
  (void)path;
  (void)max_bytes;
  return MakeError(ErrorCode::IoError, "this build only supports Windows");
#endif
}

Result<bool> RegistryStore::WriteFileAtomic(const std::filesystem::path& path,
                                           const std::vector<std::byte>& bytes) const {
#ifdef _WIN32
  const std::filesystem::path temp =
      path.parent_path() /
      (path.filename().wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId()));
  HANDLE handle = CreateFileW(temp.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return MakeError(ErrorCode::IoError, "cannot create temporary file", temp.string());
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const DWORD want =
        static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1u << 20));
    DWORD written = 0;
    if (!WriteFile(handle, bytes.data() + offset, want, &written, nullptr)) {
      CloseHandle(handle);
      DeleteFileW(temp.wstring().c_str());
      return MakeError(ErrorCode::IoError, "write failed", temp.string());
    }
    offset += written;
  }
  if (!FlushFileBuffers(handle)) {
    CloseHandle(handle);
    DeleteFileW(temp.wstring().c_str());
    return MakeError(ErrorCode::IoError, "flush failed", temp.string());
  }
  CloseHandle(handle);
  if (!MoveFileExW(temp.wstring().c_str(), path.wstring().c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temp.wstring().c_str());
    return MakeError(ErrorCode::IoError, "atomic rename failed", path.string());
  }
  return true;
#else
  (void)path;
  (void)bytes;
  return MakeError(ErrorCode::IoError, "this build only supports Windows");
#endif
}

Result<std::filesystem::path> RegistryStore::GenerationPath(GenerationNumber number) const {
  return options_.directory / (std::string(kGenerationPrefix) + FormatGenerationNumber(number) +
                               std::string(kGenerationSuffix));
}

Result<StoreMeta> RegistryStore::ReadMeta() const { return LoadMetaLocked(); }

Result<StoreMeta> RegistryStore::LoadMetaLocked() const {
  const std::filesystem::path path = options_.directory / kMetaFileName;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    StoreMeta empty;
    empty.generation = GenerationNumber(0);
    empty.epoch = PublisherEpoch(0);
    empty.incarnation = Incarnation(0);
    return empty;
  }
  auto bytes = ReadFileBounded(path, options_.max_meta_bytes);
  if (!bytes.has_value()) return bytes.error();
  const std::string_view text(reinterpret_cast<const char*>(bytes.value().data()),
                              bytes.value().size());
  JsonLimits limits;
  limits.max_bytes = static_cast<std::size_t>(options_.max_meta_bytes);
  auto json = ParseJson(text, limits);
  if (!json.has_value()) return json.error();
  return ParseStoreMeta(json.value());
}

Result<StoreMeta> RegistryStore::WriteMeta(const StoreMeta& meta) const {
  const JsonValue json = StoreMetaToJson(meta);
  const std::string text = json.Dump(false);
  std::vector<std::byte> bytes(text.size());
  std::memcpy(bytes.data(), text.data(), text.size());
  const std::filesystem::path path = options_.directory / kMetaFileName;
  auto written = WriteFileAtomic(path, bytes);
  if (!written.has_value()) return written.error();
  return meta;
}

Result<RegistryStore> RegistryStore::Open(const StoreOptions& options) {
  if (options.directory.empty()) {
    return MakeError(ErrorCode::InvalidArgument, "registry data directory must not be empty");
  }
  RegistryStore store;
  store.options_ = options;

  std::error_code ec;
  if (options.mode == OpenMode::ReadWrite) {
    std::filesystem::create_directories(options.directory, ec);
    if (ec && !std::filesystem::is_directory(options.directory)) {
      return MakeError(ErrorCode::IoError, "cannot create registry data directory",
                       options.directory.string());
    }
  } else if (!std::filesystem::is_directory(options.directory)) {
    return MakeError(ErrorCode::NotFound, "registry data directory does not exist",
                     options.directory.string());
  }

  if (options.mode == OpenMode::ReadWrite) {
    const std::string key = CanonicalDirectoryKey(options.directory);
    {
      std::lock_guard<std::mutex> guard(WriterRegistryMutex());
      if (WriterRegistry().count(key) != 0) {
        return MakeError(ErrorCode::LockContention,
                         "this process already holds the registry writer lock for that "
                         "directory",
                         options.directory.string());
      }
      WriterRegistry().insert(key);
      store.held_directory_key_ = key;
    }
#ifdef _WIN32
    const std::filesystem::path lock_path = options.directory / kLockFileName;
    HANDLE handle = CreateFileW(lock_path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      store.ReleaseLock();
      return MakeError(ErrorCode::IoError, "cannot open registry lock file", lock_path.string());
    }
    OVERLAPPED overlapped{};
    if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                    &overlapped)) {
      CloseHandle(handle);
      store.ReleaseLock();
      return MakeError(ErrorCode::LockContention,
                       "another process already holds the registry writer lock",
                       options.directory.string());
    }
    store.lock_handle_ = handle;
#else
    return MakeError(ErrorCode::IoError, "this build only supports Windows");
#endif
  }

  auto meta = store.LoadMetaLocked();
  if (!meta.has_value()) return meta.error();
  StoreMeta current = meta.value();
  store.incarnation_ = current.incarnation;
  store.epoch_ = current.epoch;

  if (options.mode == OpenMode::ReadWrite) {
    auto next_incarnation = NextIncarnation(current.incarnation);
    if (!next_incarnation.has_value()) return next_incarnation.error();
    auto next_epoch = NextEpoch(current.epoch);
    if (!next_epoch.has_value()) return next_epoch.error();
    current.incarnation = next_incarnation.value();
    current.epoch = next_epoch.value();
    current.updated_at = NowTimestamp();
    store.incarnation_ = current.incarnation;
    store.epoch_ = current.epoch;
    auto written = store.WriteMeta(current);
    if (!written.has_value()) return written.error();
  }
  return store;
}

void RegistryStore::ReleaseLock() {
#ifdef _WIN32
  if (lock_handle_ != nullptr) {
    HANDLE handle = static_cast<HANDLE>(lock_handle_);
    OVERLAPPED overlapped{};
    UnlockFileEx(handle, 0, 1, 0, &overlapped);
    CloseHandle(handle);
    lock_handle_ = nullptr;
  }
#endif
  if (!held_directory_key_.empty()) {
    std::lock_guard<std::mutex> guard(WriterRegistryMutex());
    WriterRegistry().erase(held_directory_key_);
    held_directory_key_.clear();
  }
}

RegistryStore::RegistryStore(RegistryStore&& other) noexcept
    : options_(std::move(other.options_)),
      incarnation_(other.incarnation_),
      epoch_(other.epoch_),
      lock_handle_(other.lock_handle_),
      held_directory_key_(std::move(other.held_directory_key_)) {
  other.lock_handle_ = nullptr;
  other.held_directory_key_.clear();
  other.options_ = StoreOptions{};
}

RegistryStore& RegistryStore::operator=(RegistryStore&& other) noexcept {
  if (this != &other) {
    ReleaseLock();
    options_ = std::move(other.options_);
    incarnation_ = other.incarnation_;
    epoch_ = other.epoch_;
    lock_handle_ = other.lock_handle_;
    held_directory_key_ = std::move(other.held_directory_key_);
    other.lock_handle_ = nullptr;
    other.held_directory_key_.clear();
    other.options_ = StoreOptions{};
  }
  return *this;
}

RegistryStore::~RegistryStore() { ReleaseLock(); }

Result<PublishFence> RegistryStore::CurrentFence() const {
  auto meta = LoadMetaLocked();
  if (!meta.has_value()) return meta.error();
  PublishFence fence;
  fence.expected_generation = meta.value().generation;
  fence.epoch = meta.value().epoch;
  fence.incarnation = meta.value().incarnation;
  return fence;
}

Result<std::vector<GenerationNumber>> RegistryStore::ListGenerations() const {
  std::vector<GenerationNumber> out;
  std::error_code ec;
  std::filesystem::directory_iterator iterator(options_.directory, ec);
  if (ec) {
    return MakeError(ErrorCode::IoError, "cannot enumerate registry data directory",
                     options_.directory.string());
  }
  std::size_t scanned = 0;
  for (const auto& entry : iterator) {
    if (++scanned > options_.max_scan_entries) {
      return MakeError(ErrorCode::LimitExceeded,
                       "registry data directory holds more entries than the scan limit");
    }
    if (!entry.is_regular_file(ec)) continue;
    auto number = ParseGenerationFileName(entry.path().filename().string());
    if (!number.has_value()) continue;
    out.push_back(number.value());
  }
  std::sort(out.begin(), out.end());
  return out;
}

Result<LoadedGeneration> RegistryStore::LoadGeneration(GenerationNumber number) {
  auto path = GenerationPath(number);
  if (!path.has_value()) return path.error();
  auto bytes = ReadFileBounded(path.value(), options_.max_generation_bytes);
  if (!bytes.has_value()) return bytes.error();
  GenerationNumber decoded{};
  PublisherEpoch epoch{};
  Incarnation incarnation{};
  auto document = DecodeGeneration(bytes.value(), &decoded, &epoch, &incarnation);
  if (!document.has_value()) return document.error();
  if (!(decoded == number)) {
    return MakeError(ErrorCode::CorruptPersistence,
                     "generation file name disagrees with its contents", path.value().string());
  }
  auto compiled = RegistryGeneration::Compile(document.value());
  if (!compiled.has_value()) return compiled.error();
  LoadedGeneration loaded;
  loaded.id = compiled.value()->id();
  loaded.registry = compiled.value();
  return loaded;
}

Result<LoadedGeneration> RegistryStore::LoadLatest() {
  auto meta = LoadMetaLocked();
  if (!meta.has_value()) return meta.error();
  StoreMeta head = meta.value();
  RecoveryReport recovery;

  if (head.generation.IsZero()) {
    return MakeError(ErrorCode::NotFound, "registry data directory holds no published generation");
  }
  if (options_.mode == OpenMode::ReadWrite) {
    // Uncommitted debris: generation files newer than the authoritative head.
    auto listed = ListGenerations();
    if (listed.has_value()) {
      for (GenerationNumber number : listed.value()) {
        if (number > head.generation) {
          auto path = GenerationPath(number);
          if (path.has_value()) {
            std::error_code ec;
            std::filesystem::remove(path.value(), ec);
            if (!ec) recovery.discarded_uncommitted.push_back(number);
          }
        }
      }
    }
  }

  GenerationNumber candidate = head.generation;
  while (true) {
    auto loaded = LoadGeneration(candidate);
    if (loaded.has_value()) {
      loaded.value().recovery = recovery;
      if (recovery.recovered) {
        loaded.value().recovery.detail =
            "head generation was unreadable; recovered the newest verifiable generation";
      }
      return loaded;
    }
    recovery.recovered = true;
    recovery.skipped_corrupt.push_back(candidate);
    if (candidate.IsZero()) {
      return MakeError(ErrorCode::CorruptPersistence,
                       "no verifiable registry generation could be recovered",
                       loaded.error().ToString());
    }
    candidate = GenerationNumber(candidate.value() - 1);
  }
}

Result<PublishOutcome> RegistryStore::Publish(const RegistryDocument& document,
                                              const PublishFence& fence,
                                              const CancellationToken& token) {
  if (options_.mode != OpenMode::ReadWrite) {
    return MakeError(ErrorCode::ImmutableGeneration,
                     "the registry store is opened read-only; publication is not permitted");
  }
  if (token.IsCancelled()) {
    return MakeError(ErrorCode::Cancelled, "publication cancelled before it started");
  }
  auto meta = LoadMetaLocked();
  if (!meta.has_value()) return meta.error();
  const StoreMeta current = meta.value();

  if (!(fence.expected_generation == current.generation)) {
    return MakeError(ErrorCode::StaleGeneration,
                     "publisher based its generation on an outdated registry head",
                     "expected gen-" + std::to_string(fence.expected_generation.value()) +
                         " but the store is at gen-" + std::to_string(current.generation.value()));
  }
  if (!(fence.epoch == current.epoch)) {
    return MakeError(ErrorCode::StaleEpoch,
                     "publisher epoch is stale; a newer writer has taken over this directory",
                     "presented epoch " + std::to_string(fence.epoch.value()) + ", current epoch " +
                         std::to_string(current.epoch.value()));
  }
  if (!(fence.incarnation == current.incarnation)) {
    return MakeError(ErrorCode::StaleIncarnation,
                     "publisher incarnation is stale; the registry has been reopened since",
                     "presented incarnation " + std::to_string(fence.incarnation.value()) +
                         ", current incarnation " +
                         std::to_string(current.incarnation.value()));
  }

  auto next = NextGeneration(current.generation);
  if (!next.has_value()) return next.error();
  if (options_.max_generations != 0) {
    // The bound is on retained generation files, so an explicit prune releases
    // room for further publication.
    auto retained = ListGenerations();
    if (!retained.has_value()) return retained.error();
    if (retained.value().size() >= options_.max_generations) {
      return MakeError(ErrorCode::GenerationLimitReached,
                       "retained generation limit reached; prune before publishing again",
                       "retained " + std::to_string(retained.value().size()) + " of " +
                           std::to_string(options_.max_generations));
    }
  }

  RegistryDocument to_publish = document;
  to_publish.meta.generation = next.value();
  to_publish.meta.epoch = current.epoch;
  if (current.generation.IsZero()) {
    to_publish.meta.parent_digest.reset();
  } else {
    to_publish.meta.parent_digest = current.generation_digest;
  }
  to_publish.Normalize();
  if (token.IsCancelled()) {
    return MakeError(ErrorCode::Cancelled, "publication cancelled during preparation");
  }

  auto compiled = RegistryGeneration::Compile(to_publish);
  if (!compiled.has_value()) return compiled.error();

  auto encoded = EncodeGeneration(to_publish, next.value(), current.epoch, current.incarnation,
                                  options_.max_generation_bytes);
  if (!encoded.has_value()) return encoded.error();
  if (token.IsCancelled()) {
    return MakeError(ErrorCode::Cancelled, "publication cancelled before the commit point");
  }

  auto path = GenerationPath(next.value());
  if (!path.has_value()) return path.error();
  {
    std::error_code ec;
    std::filesystem::remove(path.value(), ec);
  }
  auto written = WriteFileAtomic(path.value(), encoded.value());
  if (!written.has_value()) return written.error();

  // Cancellation after the generation file exists but before the head switch
  // is still a cancellation: the file is uncommitted debris and is removed.
  if (token.IsCancelled()) {
    std::error_code ec;
    std::filesystem::remove(path.value(), ec);
    return MakeError(ErrorCode::Cancelled, "publication cancelled before the commit point");
  }

  StoreMeta updated = current;
  updated.generation = next.value();
  updated.generation_digest = compiled.value()->id().digest;
  updated.total_publications = current.total_publications + 1;
  updated.updated_at = NowTimestamp();
  updated.last_publisher = to_publish.meta.publisher;
  auto committed = WriteMeta(updated);
  if (!committed.has_value()) return committed.error();

  PublishOutcome outcome;
  outcome.id = compiled.value()->id();
  outcome.meta = updated;
  return outcome;
}

Result<std::vector<GenerationNumber>> RegistryStore::Prune(std::size_t keep) {
  if (options_.mode != OpenMode::ReadWrite) {
    return MakeError(ErrorCode::ImmutableGeneration,
                     "the registry store is opened read-only; pruning is not permitted");
  }
  auto meta = LoadMetaLocked();
  if (!meta.has_value()) return meta.error();
  auto listed = ListGenerations();
  if (!listed.has_value()) return listed.error();
  std::vector<GenerationNumber> removed;
  const std::vector<GenerationNumber>& all = listed.value();
  if (all.size() <= keep) return removed;
  const std::size_t drop = all.size() - keep;
  for (std::size_t i = 0; i < drop; ++i) {
    if (all[i] == meta.value().generation) break;
    auto path = GenerationPath(all[i]);
    if (!path.has_value()) continue;
    std::error_code ec;
    std::filesystem::remove(path.value(), ec);
    if (!ec) removed.push_back(all[i]);
  }
  return removed;
}

}  // namespace fcr
