// Fabric Compatibility Registry - Summon Software Labs
// Umbrella header for embedders. Fabric Upgrade Manager links Fabric::fcr and
// includes this file.
//
// Typical embedding:
//
//   #include <fcr/fcr.hpp>
//
//   fcr::AuthorityOptions options;
//   options.store.directory = "C:/ProgramData/Fabric/registry";
//   options.store.mode = fcr::OpenMode::ReadOnly;
//   auto authority = fcr::RegistryAuthority::Open(options);
//   auto decision = authority.value()->QueryPair(left, right);
//
#pragma once

#include "fcr/authority.hpp"
#include "fcr/cancel.hpp"
#include "fcr/client.hpp"
#include "fcr/component.hpp"
#include "fcr/digest.hpp"
#include "fcr/document.hpp"
#include "fcr/error.hpp"
#include "fcr/identity.hpp"
#include "fcr/json.hpp"
#include "fcr/protocol.hpp"
#include "fcr/registry.hpp"
#include "fcr/rule.hpp"
#include "fcr/server.hpp"
#include "fcr/store.hpp"
#include "fcr/taxonomy.hpp"
#include "fcr/transport.hpp"
#include "fcr/validation.hpp"
#include "fcr/version.hpp"

namespace fcr {

// Version of the runtime, reported by the CLI and the service handshake.
inline constexpr const char* kRuntimeVersion = "1.0.0";

}  // namespace fcr
