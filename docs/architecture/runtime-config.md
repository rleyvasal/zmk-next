# Runtime configuration architecture

## Product promise

Users can dynamically configure every behavior instance supported by their
installed ZMK Next firmware.

This is a runtime-configuration platform, not a runtime compiler. Firmware
continues to contain all behavior implementations and all hardware integration.
Runtime configuration creates and connects bounded instances of those compiled
engines.

## Non-negotiable rules

1. Devicetree and Kconfig define hardware, enabled engines, defaults, and
   limits.
2. Runtime configuration defines user-created instances of compiled engines.
3. No runtime-loaded C code, Devicetree nodes, or Zephyr devices.
4. Every user update is validated as a complete configuration generation.
5. Never mutate the active configuration in place.
6. Persist a complete generation before activating it.
7. Activate only when keyboard input is safe.
8. The persistent-flash format is private to firmware.
9. The protobuf protocol is the public client/firmware contract.
10. All pools and resource limits remain fixed at firmware build time.

## Terminology

| Term | Meaning |
| --- | --- |
| Capability | A feature or limit compiled into a firmware build. |
| Runtime object | A user-defined instance of a compiled engine, such as a macro, hold-tap, tap-dance, or mod-morph. |
| Action reference | A compiled behavior plus parameters, or a stable runtime-object ID. |
| Snapshot | One complete serializable runtime configuration. |
| Generation | One validated version of a snapshot. |
| Draft | Client-side or firmware-staged configuration that is not active. |
| Active configuration | Immutable generation used for key processing. |
| Pending configuration | Persisted valid generation waiting for keyboard-safe activation. |

Conceptually, a snapshot contains:

```text
RuntimeConfigSnapshot
  persistence_schema_version
  generation
  capability_fingerprint
  keymap overrides
  layer metadata
  runtime objects
  combo definitions
```

Runtime object IDs are stable serialized values. RAM pointers and pool indexes
are never persisted references.

## Responsibilities

This firmware repository owns:

- the runtime model, validation, fixed-capacity pools, and dispatch;
- combo indexes, safe activation, Settings persistence, and boot recovery;
- protobuf RPC handlers and capability reporting;
- firmware, power-loss, split, and resource-limit tests.

`zmk-next-messages` owns only the shared protobuf API. It must not expose
firmware persistence structures. `zmk-next-configurator` owns the editable
draft, capability-aware forms, upload flow, and human-friendly import/export;
it never writes Zephyr Settings directly.

## Compatibility

Three versions are independent:

| Version | Meaning |
| --- | --- |
| Protocol version | Firmware and configurator RPC compatibility. |
| Persistence schema version | Firmware load/migration compatibility for saved snapshots. |
| Capability fingerprint | Whether this firmware build supports the requested types, parameters, and limits. |

On connection, the configurator reads the protocol version, persistence schema
version, capability fingerprint, enabled object types, resource limits, active
generation, and the selected-layout-to-stock-position map. It must not offer
an editor for an unadvertised behavior or upload selected-layout indexes as if
they were canonical stock positions.

Both firmware and configurator pin the same exact `zmk-next-messages` release
or commit.

## Transaction and activation model

Updates are complete-snapshot uploads in the first protocol version:

```text
draft -> staging -> validated -> persisted pending idle -> active
```

The firmware stages all chunks, validates references and budgets, writes a new
generation to the inactive Settings slot, then writes that slot's manifest
last. On boot it selects the newest complete, checksum-valid, capability-
compatible generation; otherwise it uses the previous valid generation or
compiled defaults.

`ResetRuntimeConfig` uses the same transaction path: it persists an empty
runtime overlay as a new generation rather than deleting Settings entries.
After safe activation, compiled defaults are the effective keymap again while
the previous valid generation remains an on-flash recovery fallback.

A pending generation activates only when no physical key is pressed, no combo
is being captured, no hold-tap or tap-dance is undecided, no macro is running,
and no action retains the old generation. The device reports whether a commit
is active now or saved pending idle.

The current milestone tracks the logical active and pending generation, raw
physical key state, and behavior-engine blocker leases. A committed generation
contains bounded ActionRef keymap overrides; an ActionRef is either a compiled
behavior binding or a validated runtime-object ID. The keymap reads an override
only after its generation becomes active. Firmware serializes that validated
model into a private payload before A/B Settings persistence, rather than
persisting a client protobuf.

Version 0.1 accepts ordinary bindings and existing compiled layer actions with
their normal parameters. It also supports a runtime mod-morph object: on key
press, the dispatcher resolves its normal or morphed compiled action and saves
that exact choice in per-key invocation state, so key release cannot be
affected by modifier changes or a configuration activation.

Runtime macros use a fixed shared step pool and support `tap`, `press`,
`release`, explicit `wait`, and `pause-until-release` instructions. A tap uses
the firmware's compiled default tap duration. Macro `press` and `release`
instructions must balance within one macro, so a complete program cannot leave
a binding held. Each running macro holds an activation blocker until it
finishes, including while waiting or paused. Mod-morph and macro child actions
are compiled bindings in this increment; runtime-object nesting is rejected
until the object graph has a complete recursion and lifetime model. Layer
metadata remains unsupported.

Runtime combos use a fixed pool and accept a canonical list of stock logical
key positions, a timeout, optional slow-release and require-prior-idle
settings, and one ActionRef output. The active physical-layout mapping rebuilds
the runtime combo index whenever a generation activates or the selected layout
changes. This preserves saved combo positions across UI layout choices. A
runtime combo is additive to compiled Devicetree defaults in this increment;
replacing or deleting a compiled default requires explicit baseline-management
protocol support. A captured runtime combo retains an activation blocker until
its final source-key release, so its saved output binding remains valid while
the combo completes.

Runtime hold-taps reuse ZMK's existing hold-tap state machine. A runtime
object supplies compiled tap and hold actions, one of the four standard
flavors, a tapping term, and optional quick-tap and require-prior-idle times.
At press time, the engine copies both resolved child bindings and the hold-tap
settings into its per-press record; it retains an activation blocker until the
matching release has completed. Consequently a generation cannot activate
mid-decision or change the binding paired with a release. Runtime hold-taps do
not yet support nested runtime objects, positional triggers, retro-tap, or
hold-while-undecided options; those remain Devicetree-only until their runtime
semantics and lifetime rules are explicitly specified.

Runtime tap-dances use a fixed shared action pool. Actions are numbered
consecutively from one and each supplies compiled tap and hold bindings. On
timeout or interruption, the engine selects the tap binding if the dance key
is up and the hold binding if it is still down; it snapshots that selected
binding for the matching release. The active generation remains blocked while
the dance is collecting taps or holding its selected binding, so the action
pool remains immutable for the invocation's lifetime. Runtime object nesting
is not yet supported inside tap-dance actions.

## Runtime-editable scope

Once their engines are implemented, ordinary key bindings, layer actions,
combos, macros, mod-morphs, hold-taps, and tap-dances are editable live.
Arbitrary behavior C code, layer metadata, a new hardware/layout matrix, and
resource growth beyond build-time pools are not.

## Delivery order

1. Capability endpoint, complete-snapshot staging, validation, and A/B
   persistence with no live dispatch.
2. Safe activation and ordinary binding/layer overlay.
3. Action references, the runtime dispatcher, and mod-morphs.
4. Macros with timing and pause-until-release.
5. Combos.
6. Hold-taps with shared per-invocation state and activation blocking.
7. Tap-dances with per-invocation selected bindings and activation blocking.
8. Active-snapshot readback and transactional stock reset.

Every increment retains compiled defaults as a recovery fallback and must pass
decoder, validation, power-loss, activation, split, and resource-limit tests
before hardware rollout.
