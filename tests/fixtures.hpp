// Fabric Compatibility Registry - Summon Software Labs
// Shared, deterministic fixtures for the validation suite.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "fcr/fcr.hpp"
#include "test_support.hpp"

namespace fcr::test {

// Deterministic pseudo random generator (SplitMix64). Seeded explicitly in
// every randomized test so failures are reproducible.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}
  std::uint64_t Next() {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  std::uint32_t Below(std::uint32_t bound) {
    return bound == 0 ? 0 : static_cast<std::uint32_t>(Next() % bound);
  }
  bool Chance(std::uint32_t percent) { return Below(100) < percent; }

 private:
  std::uint64_t state_;
};

std::string UniqueTempDirectory(const std::string& tag);

// Best-effort directory removal for test teardown. Never throws: a directory
// may still hold an open handle while the owning object is alive, and every
// registered temporary directory is removed again at process exit, after all
// owners have been destroyed.
void RemoveDirectoryQuietly(const std::string& path);

// A fully populated, valid generation used across the suite.
RegistryDocumentBuilder MakeStandardBuilder();
RegistryDocument MakeStandardDocument();

// Handy identity helpers (they cannot fail for these constant values).
ComponentFamilyId TransportFamily();
ComponentFamilyId ComputeFamily();
ComponentKindId NicKind();
ComponentKindId SwitchKind();
ComponentKindId GpuKind();
ProtocolId RdmaProtocol();
ProtocolId LinkProtocol();
SchemaId ConfigSchema();
CapabilityId RoceCapability();
CapabilityId LanesCapability();
CapabilityId ChannelCapability();
CapabilityId ApiLevelCapability();
FeatureId SecureBootFeature();
HardwareClassId SmartNicGen3();
HardwareClassId SmartNicGen1();

Rule MakeRule(const std::string& id, RuleOutcome outcome);

// Component specs. Closures default to Closed in fixtures so that the common
// path exercises decisive rules; open-closure behaviour has dedicated tests.
ComponentSpec MakeNic(const std::string& instance, const std::string& version);
ComponentSpec MakeSwitch(const std::string& instance, const std::string& version);
ComponentSpec MakeGpu(const std::string& instance, const std::string& version);
void CloseKnowledge(ComponentSpec* spec);

}  // namespace fcr::test
