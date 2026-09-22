# Fabric Compatibility Registry

A standalone, vendor-neutral compatibility authority for
**Summon Software Labs Fabric OS**.

The registry owns *compatibility knowledge* and *compatibility decisions*. It answers one
question, and answers it the same way every time:

> Given a set of typed components, versions and capabilities, and a specific registry
> generation, may they safely interoperate — and why?

It is deliberately **not** an upgrade executor, **not** a configuration distributor, and
**not** a capability-discovery agent. It never touches a fabric, never plans an upgrade and
never guesses a fact it was not given.

Version 1.0.0. Apache License 2.0.

---

## Contents

1. [Systems boundary](#systems-boundary)
2. [Validated platform](#validated-platform)
3. [Build, test, install](#build-test-install)
4. [Quick start](#quick-start)
5. [Domain model](#domain-model)
6. [Rule model](#rule-model)
7. [Decision semantics](#decision-semantics)
8. [Precedence and conflict resolution](#precedence-and-conflict-resolution)
9. [Negotiation](#negotiation)
10. [Static validation](#static-validation)
11. [Canonical form and the generation digest](#canonical-form-and-the-generation-digest)
12. [Persistence, generations and fencing](#persistence-generations-and-fencing)
13. [Service protocol](#service-protocol)
14. [Library interface](#library-interface)
15. [Resource bounds](#resource-bounds)
16. [Concurrency, shutdown and the lock audit](#concurrency-shutdown-and-the-lock-audit)
17. [Validation and proof surfaces](#validation-and-proof-surfaces)
18. [Benchmarks](#benchmarks)
19. [Exit codes](#exit-codes)
20. [Known limitations](#known-limitations)
21. [License](#license)

---

## Systems boundary

| Owned by this repository | Not owned by this repository |
| --- | --- |
| The compatibility taxonomy (families, kinds, protocols, schemas, capabilities, features, hardware classes) | Discovery of what a component actually is or has |
| Compatibility rules and their provenance | Executing an upgrade, rollback or drain |
| Deterministic compatibility decisions and explanations | Distributing configuration or firmware |
| Immutable, digest-addressed registry generations | Choosing *when* to apply a change |
| Stale-publisher and stale-client fencing | Fabric-wide orchestration |

A caller supplies observed component facts, including an explicit statement of **how
complete** those facts are. The registry decides; it never fills a gap with an assumption.

---

## Validated platform

Windows x64 with MSVC 19.44 (Visual Studio 2022). The CMake project **refuses to configure**
on any other toolchain rather than producing an unvalidated build. The reasons are concrete:

* persistence uses Win32 byte-range locks and write-through atomic renames;
* the transport uses Winsock;
* the independent-process tests use the Win32 process and pipe APIs.

---

## Build, test, install

Requires CMake 3.25+, Ninja, and Visual Studio 2022 with the C++ workload.

    scripts\build.bat build Release
    scripts\build.bat build-debug Debug
    build\tests\fcr_tests.exe          # Release
    build-debug\tests\fcr_tests.exe    # Debug

Both configurations are built with `/W4 /WX /permissive- /utf-8 /EHsc /Zc:__cplusplus`.
First-party warning count is zero. The Debug configuration additionally runs with the debug
CRT heap validation and STL iterator debugging that MSVC enables by default.

Install and consume from an independent project:

    cmake --install build --prefix C:\fcr
    cmake -S examples\downstream-consumer -B out -G Ninja -DCMAKE_PREFIX_PATH=C:\fcr
    cmake --build out

The consumer project is intentionally outside the runtime build: it only sees the installed
headers and the exported `Fabric::fcr` target.

Build options: `FCR_BUILD_TOOLS`, `FCR_BUILD_TESTS`, `FCR_BUILD_EXAMPLES`,
`FCR_BUILD_BENCHMARKS`, `FCR_WARNINGS_AS_ERRORS`.

---

## Quick start

    # Static validation needs no registry directory at all.
    fcrctl validate registry.json

    # Publish a generation. --expected-generation is the fence: the generation the
    # publisher based its change on.
    fcrctl publish registry.json --data-dir C:\fcr-data --expected-generation 0

    fcrctl status --data-dir C:\fcr-data
    fcrctl query pair --left nic-a.json --right nic-b.json --data-dir C:\fcr-data
    fcrctl diff --from 1 --to 2 --data-dir C:\fcr-data
    fcrctl provenance --rule nic-requires-roce --data-dir C:\fcr-data

    # Or run the authority as a service and talk to it.
    fcr-registryd --data-dir C:\fcr-data --workers 4
    fcrctl status --endpoint 127.0.0.1:PORT

A component observation looks like this:

    {
      "instance": "node-a-nic0",
      "family": "fabric.transport",
      "kind": "rdma.nic",
      "version": "2.4.1",
      "hardware_class": "smartnic.gen3",
      "knowledge": { "capabilities": "closed", "protocols": "closed", "schemas": "closed" },
      "capabilities": { "rdma.rocev2": true, "link.lanes": 16 },
      "protocols": { "fabric.rdma": ["2.0.0", "2.1.0"] },
      "schemas": { "fabric.config": ["1.2.0"] },
      "features": ["secure.boot"]
    }

`knowledge` is the point of the whole design. `closed` means "this list is everything the
component has"; `open` means "this list is what we managed to observe". Open knowledge turns
*absent* into UNKNOWN instead of INCOMPATIBLE, and it is the default.

---

## Domain model

Every important domain object is a distinct C++ type. A capability identifier cannot be
passed where a component kind is expected; a generation counter cannot be passed where an
epoch is expected.

| Identity | Type | Notes |
| --- | --- | --- |
| Component family | `ComponentFamilyId` | qualified name, e.g. `fabric.transport` |
| Component kind | `ComponentKindId` | vendor-neutral role, e.g. `rdma.nic` |
| Version | `SemVersion` | SemVer 2.0.0, checked 32-bit components |
| Version range | `VersionRange` | canonical, sorted, disjoint interval set |
| Protocol / schema | `ProtocolId`, `SchemaId` | concrete supported version lists |
| Capability | `CapabilityId`, `CapabilityValue` | typed: flag, integer, text, version |
| Feature | `FeatureId` | boolean fact; a distinct type from a flag capability |
| Hardware class | `HardwareClassId` | where hardware identity genuinely matters |
| Rule | `RuleId`, `RuleRevision` | one revision per identity per generation |
| Registry generation | `GenerationId` | ordinal **and** SHA-256 content digest |
| Epoch / incarnation | `PublisherEpoch`, `Incarnation` | writer and boot fencing |
| Decision | `DecisionId` | SHA-256 over the semantic query, generation and evidence |

`Timestamp` is explicit everywhere: no part of the deterministic core reads a clock. The
store records timestamps; decisions never depend on them.

---

## Rule model

A rule declares one relation between two selected components (pair arity) or across a whole
set (set arity):

    {
      "id": "nic-requires-roce",
      "revision": 3,
      "priority": 20,
      "origin": "standard",
      "symmetry": "symmetric",
      "arity": "pair",
      "outcome": "compatible",
      "left":  { "kind": "rdma.nic", "version": ">=2.0.0 <3.0.0" },
      "right": { "kind": "rdma.nic", "version": ">=2.0.0 <3.0.0" },
      "constraints": [
        { "type": "requires_capability", "side": "left",  "capability": "rdma.rocev2" },
        { "type": "requires_capability", "side": "right", "capability": "rdma.rocev2" },
        { "type": "requires_protocol", "side": "left",  "protocol": "fabric.rdma", "range": "^2.0.0" },
        { "type": "requires_protocol", "side": "right", "protocol": "fabric.rdma", "range": "^2.0.0" }
      ],
      "negotiation": "highest_common",
      "on_unmet_requirement": "incompatible",
      "on_indeterminate": "unknown",
      "lifecycle": { "state": "active" },
      "provenance": { "publisher": "platform-engineering", "source": "matrix-2026Q1",
                      "recorded_at": "2026-03-01T00:00:00Z" },
      "rationale": "2.x adapters interoperate when both sides speak RoCEv2"
    }

Model coverage:

* **exact compatibility** — an exact version selector or an `=1.2.3` range;
* **ranges, minimums and maximums** — full SemVer range syntax plus `version_at_least` and
  `version_at_most` constraints;
* **required and forbidden capabilities / features / protocols / schemas / hardware classes**;
* **protocol and schema negotiation** — per-rule policy: `highest_common`,
  `lowest_common`, `exact_required`;
* **conditional constraints** — the declared outcome only applies when every constraint is
  satisfied, and three explicit policies say what happens otherwise;
* **asymmetric compatibility** — `left_to_right` / `right_to_left` rules apply in one
  orientation only, and the explanation records the orientation that matched;
* **known-incompatible combinations** — an INCOMPATIBLE rule with an explicit rationale;
* **capability implications** — "if X is present then Y must be", with cycle detection;
* **lifecycle and provenance** — active / deprecated / retired, superseding references,
  publisher, source, reference, timestamp and change note per rule.

Constraints are stored in a canonical order, so two rules written in different orders are
byte-identical after normalization and produce the same digest.

---

## Decision semantics

Three outcomes, never two:

| Outcome | Meaning |
| --- | --- |
| `compatible` | A rule reached a positive verdict from definitely-satisfied evidence |
| `incompatible` | A rule reached a negative verdict, or a requirement is definitely unmet |
| `unknown` | Nothing decided it, or the evidence is incomplete |

**UNKNOWN is never collapsed into COMPATIBLE.** Concretely:

* no matching rule at all → UNKNOWN (`no_matching_rule`);
* a matched rule needs a capability the component does not list and the component's
  capability knowledge is `open` → UNKNOWN;
* the same capability with `closed` knowledge → INCOMPATIBLE, because absence is then
  evidence of absence;
* every matched rule declining to apply (`skip` policy) → UNKNOWN;
* a component kind the taxonomy does not declare → UNKNOWN, not an error and not COMPATIBLE.

A query returns an inspectable explanation, not just a verdict:

    decision 4f63d437c20e1ca2b098f271b14ec7b7d4a4498e96decaa10c4f93f07df0994f
      outcome: compatible
      reason: decided_by_rule
      generation: gen-1:6cf5f6414d4ea1a8985a1f0ac74be385f7097c755f92261301a8336ff1294b9e
      arity: pair
      subject[0]: rdma.nic 2.4.1 (family=fabric.transport, instance=node-a-nic0)
      subject[1]: rdma.nic 2.3.0 (family=fabric.transport, instance=node-b-nic0)
      deciding_rule: rdma-generation-window@r1
      matched_rules: 1
        [0] rdma-generation-window@r1 priority=10 specificity=44 origin_rank=0 status=satisfied ...
            requires_capability side=left status=satisfied
            requires_capability side=right status=satisfied
      negotiation: policy=highest_common
        protocol fabric.rdma status=agreed selected=2.1.0 window=[2.1.0]

The explanation lists every matched rule with its precedence tier, its per-constraint status,
whether it decided, which rules it overrode, the unmet requirements, the negotiated versions,
and the generation and authority the decision came from.

`DecisionId` is a SHA-256 over the *semantic* query. Instance names, labels and vendor
identity are excluded, so the same question asked about a renamed component yields the same
identifier; the same question under a different generation yields a different one.

---

## Precedence and conflict resolution

Precedence is a total order with a documented key:

1. **priority** — the explicit signed integer on the rule (higher wins);
2. **specificity** — derived: constrained selector dimensions, exact versions weighted over
   ranges, plus the number of constraints (higher wins);
3. **origin rank** — local override > operator > vendor > standard;
4. **rule identity** — canonical `id@revision`, ascending, as the final tie-break.

Items 1–3 form the **precedence tier**. Two rules in the *same tier* whose regions overlap
and whose outcomes disagree are a **publication-blocking conflict**: no arbitrary tie-break
is allowed to decide a compatibility question. Different tiers are a legitimate override and
are recorded in the explanation as `overridden`.

This is why a conflicting rule set can never become authoritative:
`RegistryGeneration::Compile` refuses to produce a generation whose validation report
contains any error, and every publication path goes through it. A store asked to publish an
invalid document writes nothing at all.

---

## Negotiation

Protocol and schema support is a list of concrete versions, so negotiation produces a
concrete answer rather than an unbounded interval:

* both sides support the protocol and share a version → `agreed` with the selected
  version (`highest_common` by default, or the deciding rule's policy);
* both sides support it but share nothing → `no_overlap`;
* one side's protocol knowledge is `open` and does not mention it → `indeterminate`;
* one side's knowledge is `closed` and does not mention it → `not_supported`.

`exact_required` only accepts a window of exactly one version. The negotiation policy of the
deciding rule governs the reported result; when no rule decides, `highest_common` is used.
Set queries negotiate across every member.

---

## Static validation

`ValidateRegistryDocument` returns a report of errors, warnings and information items, each
with a code, a path and, where applicable, the rule it concerns. Errors block publication.

| Code | Detects |
| --- | --- |
| `duplicate_rule_identity` | the same rule identity declared twice in one generation |
| `conflicting_rules` | same precedence tier, overlapping region, disagreeing outcomes |
| `impossible_version_range` | a range that matches no version at all |
| `unsatisfiable_constraints` | requires and forbids the same fact on one side; minimum above maximum |
| `undefined_family/kind/protocol/schema/capability/hardware_class/feature` | references outside the taxonomy |
| `kind_family_mismatch` | a selector or spec pairing a kind with the wrong family |
| `capability_type_mismatch`, `unsupported_comparison` | a literal or operator that does not fit the declared capability type |
| `unreachable_rule` | a rule that can never decide anything |
| `implication_cycle`, `self_implication` | cycles in the capability-implication graph |
| `set_rule_uses_right_side`, `set_rule_declares_right_selector` | structural errors in set-arity rules |
| `deprecated_without_successor`, `superseded_by_missing`, `retired_rule_declared` | lifecycle consistency |
| `unused_capability`, `unreferenced_taxonomy_entry` | declarations nothing references (information) |
| `reachability_analysis_skipped` | the analysis budget was exceeded (information); the report marks itself approximate |

### How unreachable rules are decided

A rule is unreachable when higher-precedence active rules cover its whole region. The
analysis is deliberately sound rather than aggressive:

* only rules whose policies never skip them can shadow — a rule that may decline to apply
  cannot guarantee that a lower rule never decides;
* non-version selector dimensions must subsume (family, kind, hardware class), with mirrored
  orientations considered for symmetric rules;
* the two version dimensions are then covered by a coordinate-compressed grid over the union
  of the candidate boxes, which is exact for the modeled region;
* the whole analysis is bounded. If a generation declares more than 2048 rules, or a rule has
  more than 64 shadowing candidates, or the grid would exceed 4096 cells, the analysis falls
  back to single-shadower subsumption and reports `reachability_analysis_skipped` with
  `reachability_exhaustive == false`.

Constraints are not modeled in the coverage geometry, so a rule that is unreachable in
practice may still be reported as reachable. The detector never reports a false unreachable
rule.

---

## Canonical form and the generation digest

A generation is identified by its ordinal **and** SHA-256 over its canonical serialization.
The canonical form is deterministic JSON: keys sorted, no insignificant whitespace, every
collection in a defined order, integers written exactly.

Consequences that are tested as properties:

* adding rules in any order produces byte-identical canonical JSON and the same digest;
* shuffling a rule's constraints produces the same digest;
* serializing a document and parsing it back reproduces the same canonical bytes;
* a stored generation file is re-verified by recomputing the digest of the decoded document
  and comparing it with the stored one.

SHA-256 and CRC-32C are implemented in this repository and checked against published test
vectors, including the one-million-character SHA-256 vector and the standard CRC-32C vectors.

---

## Persistence, generations and fencing

A data directory holds:

    registry.meta                 head metadata: current generation + digest, epoch,
                                  incarnation, publication count, integrity digest
    registry.lock                 Win32 byte-range lock held by the writer
    gen-00000000000000000001.fcr  immutable generation snapshots
    ...

Generation file layout (little endian, 96-byte header):

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | magic FCRG |
| 4 | 2 | format version |
| 6 | 2 | header size |
| 8 | 8 | generation ordinal |
| 16 | 8 | publisher epoch |
| 24 | 8 | publisher incarnation |
| 32 | 8 | payload length |
| 40 | 32 | payload SHA-256 |
| 72 | 4 | CRC-32C over header bytes [0,72) |
| 96 | .. | canonical document JSON |

Publication is atomic. The generation file is written to a temporary name, flushed with
`FlushFileBuffers`, renamed into place, and only then does the head metadata switch over,
itself through a write-through replacement. **The metadata update is the commit point.**

Recovery is conservative and explicit:

* a corrupt head generation is skipped and the newest verifiable generation is loaded; every
  skipped ordinal is reported in a `RecoveryReport`;
* generation files newer than the authoritative head are uncommitted debris and are deleted,
  not adopted;
* the head metadata carries its own integrity digest and is rejected if it does not match;
* a decoded document whose recomputed digest differs from the stored digest is rejected;
* nothing is ever silently promoted to current merely because it deserialized.

Fencing has three independent inputs, all of which must match: the **expected generation**
(the head the publisher read), the **publisher epoch** (advanced every time a directory is
opened for writing) and the **incarnation** (advanced on every open). A journal that tries to
publish on top of a newer head is refused with `stale_generation`; a writer from a previous
opening is refused with `stale_epoch` or `stale_incarnation`. The same values are checked
against a caller-supplied stamp on the service protocol, so a client that reconnects after a
restart is fenced rather than served from its stale beliefs.

Retention is bounded by `max_generations` (default 512), counted in *retained generation
files*, not ordinals: an explicit `prune` releases room for further publication, and pruning
never removes the authoritative generation.

---

## Service protocol

`fcr-registryd` serves the same operations the library exposes. Frames are length-prefixed
and checksummed:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | magic FCRF |
| 4 | 2 | frame format version |
| 6 | 2 | frame kind |
| 8 | 4 | payload length |
| 12 | 8 | correlation identifier |
| 20 | 4 | CRC-32C over header bytes [0,20) |
| 24 | 4 | CRC-32C over the payload |
| 28 | .. | canonical JSON payload |

The handshake negotiates the protocol version and the maximum frame size (the minimum of both
sides), and returns the service incarnation, current generation and publisher epoch. A client
that presents a stale incarnation or generation in a request `stamp` is refused before the
request is executed.

Operations: `ping`, `status`, `validate`, `publish`, `query_pair`, `query_set`,
`diff`, `replay`, `provenance`, `generations`, `prune`, `shutdown`. Responses are
an `ok` envelope carrying either `result` or a typed `error` with a code, a message and
detail, so every failure crosses the wire as a code rather than a message to be parsed.

A `replay` request evaluates a corpus of queries under two generations and reports exactly
which decisions changed — completed work, not a prediction.

---

## Library interface

    #include <fcr/fcr.hpp>

    fcr::AuthorityOptions options;
    options.store.directory = "C:\\ProgramData\\Fabric\\registry";
    options.store.mode = fcr::OpenMode::ReadOnly;      // readers never take the writer lock
    auto authority = fcr::RegistryAuthority::Open(options);
    if (!authority.has_value()) { /* authority.error() is typed */ }

    auto decision = authority.value()->QueryPair(left, right);
    std::cout << decision.value().narrative;

`RegistryAuthority` is the whole surface Fabric Upgrade Manager needs: open, query pair,
query set, snapshot a generation, validate, simulate, publish, diff, replay, provenance,
status, stamp checking and prune. `OpenInMemory` provides the same behaviour without
persistence, for embedding and tests.

Sources of `Result<T>` return their failures as values; nothing on a runtime path throws.
`value()` on a failed result throws `std::bad_variant_access`, the standard checked-access
behaviour, and is only reached through a caller's own unchecked access.

---

## Resource bounds

Every externally influenced surface is bounded:

| Surface | Bound |
| --- | --- |
| JSON document | 8 MiB, depth 64, 1 MiB per string, 2^20 elements |
| Registry document | 65 536 rules, 128 constraints and 128 implications per rule |
| Taxonomy | 256 families, 1024 kinds, 4096 capabilities, 512 protocols/schemas, 256 hardware classes, 1024 features |
| Component spec | 512 capabilities, 128 protocols, 128 schemas, 256 features |
| Version | 32-bit components, 512-byte text, 32 prerelease identifiers |
| Version range | 4096 bytes, 64 OR groups, 64 comparators per group |
| Generation file | 64 MiB, header-declared length checked against the file size |
| Set query | 64 members, so at most 2016 pairwise evaluations |
| Decision evidence | 256 rule entries, always including the deciding rule; truncation is reported |
| Negotiated subjects | 64 protocols and 64 schemas per decision |
| Service connections | 32 concurrent, 256 queued, 4 workers by default (configurable, with hard caps) |
| Frame payload | 1 MiB default, negotiated down to the client's maximum |
| Generations retained | 512 by default; publication is refused rather than growing without bound |

Lengths and sizes are computed with checked arithmetic, and out-of-range values are refused
with `limit_exceeded` or `arithmetic_overflow` rather than truncated.

---

## Concurrency, shutdown and the lock audit

### Ownership

* One `shared_mutex` guards the current snapshot, the retained history and the cached head
  metadata of a `RegistryAuthority`.
* Queries take the shared lock, copy an immutable `shared_ptr<const RegistryGeneration>` and
  return. No lock is held while a caller inspects evidence, and generations are immutable, so
  a reader can never observe a half-published rule set.
* Publication validates, compiles and encodes **outside every lock**, then takes the exclusive
  lock only to re-check the fence and swap the head. Racing publishers therefore produce
  exactly one winner and a precise `stale_generation` for every loser.
* The store's OS-level writer lock is taken once, during `Open`, and never while the
  authority mutex is held by another path. Lock order is always authority mutex, then store
  file handle, and nothing acquires them in the other order.
* No callback, no logging sink and no event emission runs under a lock. The server's request
  handlers run with no server lock held at all.

### Re-entrancy and deadlock review

Checked explicitly and found clean:

* **read→write upgrades**: no path upgrades a shared lock to exclusive while holding it; the
  publish path releases every shared lock before taking the exclusive one.
* **callbacks beneath locks**: the only stored callable is the server's shutdown observer,
  invoked from `BeginShutdown` after every lock has been released.
* **nested acquisition**: `connections_mutex_` and `queue_mutex_` are never held
  simultaneously; the connection registry is updated under one and the queue under the other,
  in sequence.
* **shutdown joins**: the acceptor and the workers are joined unconditionally, with no polling
  and no timeout. The join can only be reached after admission has stopped and every worker
  has been woken.
* **cancellation**: an operation checks its token before starting, during preparation and again
  immediately before the commit point. A cancelled publication leaves no committed generation
  and removes its temporary file.

### How shutdown actually wakes a blocked worker

`shutdown()` on a socket does **not** reliably wake a `recv()` blocked in another thread
on this platform, and closing that socket from another thread would race with the reader.
Closing the listening socket from another thread to break `accept()` is equally
unsupported. Two mechanisms are used instead, both verified by tests:

* each worker owns a private loopback socket pair; its blocking wait is a `select()` over
  {connection, wakeup}. Shutdown writes one byte to each wakeup socket, so the worker returns
  immediately, checks the stop flag, and closes its **own** connection socket. Connection
  sockets are never touched by another thread.
* the acceptor is woken by a real loopback connection to its own listener, which it accepts,
  recognises and closes.

The consequences are visible in the tests: `shutdown_wakes_idle_connections_and_workers`
holds three connections open with no traffic at all and still shuts down cleanly, and
`server_lifecycle_repeats_cleanly` runs six full start/stop cycles and checks that accepted,
completed, active and queued counters all return to sane baselines.

Each worker serves one connection at a time, so `worker_threads` bounds how many
connections can complete a handshake concurrently; further admitted connections wait in the
bounded queue.

---

## Validation and proof surfaces

### What is proven for real

| Surface | How it is proven |
| --- | --- |
| SemVer parsing, precedence and ranges | table-driven tests plus randomized property tests over membership, union, intersection and ordering |
| Canonicalization and digests | order-independence properties, round-trip properties, published SHA-256 and CRC-32C vectors |
| Static validation | table-driven tests for every diagnostic code, including union coverage of dead rules and implication cycles |
| Decision semantics | table-driven tests for precedence, asymmetry, conditional policies, unknown versus incompatible, negotiation and set aggregation |
| Determinism | repeated evaluation, shuffled construction orders, recompiled registries, stable decision identifiers |
| Persistence | real files on disk: publish, chain verification, restart, retention, pruning, byte-level corruption and recovery |
| Corruption | every single-byte flip in an encoded generation is detected, plus truncated, oversized and random-noise inputs |
| Malformed input | several thousand mutated documents and corrupted frames; every outcome is a typed error, never a crash |
| Concurrency | four readers during thirty publications, eight racing publishers, cancellation races, repeated open/close cycles |
| Randomized state machine | seeded random operation sequences over a persistent authority, with invariants checked after every step |
| Real independent processes | the daemon is started with `CreateProcess`, driven over real loopback TCP, hard-killed with `TerminateProcess`, restarted, and a client holding the dead incarnation is fenced |
| Real multi-process races | two `fcrctl` processes publish against one daemon with the same expected generation: exactly one wins |
| Cross-process writer exclusion | a second authority opened for writing on the same directory, in-process and from a separate process, is refused |
| Packaging | an independent downstream project builds and runs against the installed package through `find_package` |
| Build hygiene | Release and Debug with `/W4 /WX`; zero first-party warnings |
| Shutdown and cancellation | idle connections, in-flight publishes, cancelled publications and repeated start/stop cycles |

140 tests run to completion in about eight seconds with **no timeouts of any kind**. A test
that hangs is a defect in the runtime, and three of them were found and fixed that way during
development.

### Synthetic

The component vocabulary used in the tests, examples and benchmarks (`rdma.nic`,
`fabric.switch`, `smartnic.gen3`, `fabric.rdma`, `rdma.rocev2` …) is a **synthetic
domain model**. It exercises every rule, constraint and negotiation shape the runtime
supports. No claim is made that these particular compatibility rules describe any real
product. The registry is vendor-neutral by construction: it consumes whatever taxonomy and
rules a deployment publishes.

### Unsupported — not claimed, not tested

* **AddressSanitizer**: only the 32-bit ASan runtime is installed with this Visual Studio
  installation; x64 ASan is unavailable, so this repository does **not** claim ASan coverage.
  The Debug configuration runs with the debug CRT heap and STL iterator debugging instead.
* **Non-Windows platforms**: the build refuses to configure anywhere else. There is no
  validated Linux, macOS or POSIX build.
* **Hardware behaviour**: no RDMA, NVLink, multi-GPU, switch-vendor or firmware behaviour is
  claimed or tested. The runtime models compatibility knowledge; it never talks to hardware.
* **Multi-machine distribution**: the service uses loopback TCP between processes on one host.
  No multi-host, clustering or network-partition behaviour is claimed.
* **Timing**: no latency guarantee, throughput guarantee or scheduling bound is claimed.

---

## Benchmarks

`fcr-bench` measures completed work only — every figure is the throughput of operations
that finished. Representative run on the development machine (Release, MSVC 19.44, 512-rule
registry, 20 000 iterations):

| Measurement | Throughput | p50 | p99 |
| --- | --- | --- | --- |
| Registry validate + compile (512 rules) | 25 ops/s | 40.4 ms | 46.8 ms |
| Pair compatibility decision | 52 300 ops/s | 14.5 us | 39.8 us |
| Canonical serialization + SHA-256 | 288 ops/s | 3.4 ms | 5.3 ms |
| Atomic persistent publication | 29 ops/s | 33.8 ms | 45.5 ms |
| Loopback TCP request/response round trip | 32 100 ops/s | 21.7 us | 99.4 us |
| Remote pair decision over loopback TCP | 10 100 ops/s | 83.1 us | 246.1 us |

Reading these honestly:

* decisions are cheap; the cost is in reading knowledge, not in using it;
* publication is dominated by canonical serialization and the durability flush — the intended
  trade for an authority whose generations must be immutable and verifiable;
* registry compilation is deliberately thorough (conflict detection, validity checking and
  reachability analysis) and is a publication-time cost, not a query-time cost.

---

## Exit codes

`fcrctl` distinguishes "the command failed" from "the answer was no", which is what makes it
usable in scripts:

| Code | Meaning |
| --- | --- |
| 0 | the command completed and the answer is affirmative |
| 1 | the command failed: usage, I/O, or a typed runtime error |
| 2 | the command completed and the answer is negative (not publishable, incompatible, changed) |
| 3 | the command completed and the answer is UNKNOWN. An unknown compatibility answer is deliberately distinct from an error: it means the registry does not know |

---

## Known limitations

* **Windows x64 only**, for the concrete reasons above.
* **Unreachable-rule detection is sound but not complete.** Constraints are not part of the
  coverage geometry, and the analysis is budgeted. Approximate results are reported as such
  rather than presented as proof.
* **Negotiation selects from concrete version lists.** A protocol declared only as an interval
  with no enumerated versions yields no selected version; the status makes that explicit
  instead of inventing a value.
* **One worker serves one connection at a time.** Many idle clients need proportionally many
  workers; the admission queue bounds the damage but a queued client waits for a worker.
* **A generation file is at most 64 MiB** and a generation at most 65 536 rules; larger rule
  sets need multiple generations or a future format version.
* **Pruning is explicit.** Retention never silently deletes history; an operator must ask.
* **No authentication or transport security** on the service. It binds loopback by default and
  is intended for a single host; the protocol is not a hardened network service.
* **ASan is unavailable** in this toolchain, as noted above.

---

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
