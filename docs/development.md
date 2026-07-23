# Development

This page describes the maintained implementation and verification surfaces. Normal users should
start with [Usage](usage.md).

## Source Layout

```text
common/
  argv/       byte-preserving argv and command-line helpers
  control/    daemon endpoint policy and control-session service
  launch/     launch CLI and descriptor execution
  process/    POSIX and Windows process implementations
  protocol/   JSON control messages, GPU selectors, framing, and lifecycle
  sidecar/    sidecar request model, registry, placement, and input queue
  transport/  TCP connection/listener and framed message channel
  util/       bounded JSON implementation
  vkrpc/      Vulkan RPC operations, normalization, validators, mock backend, profiling

linux/
  icd/        Vulkan ICD loaded by the Linux Vulkan loader
  launcher/   application wrapper and private-session lifecycle
  sidecar/    X11 WM, capture, input, cursor, popup, and geometry bridge
  observe/    source-layer PNG capture client
  canary/     native Vulkan and GL behavior probes

windows/
  supervisor/ daemon, adapter probe, worker spawning, and monitoring
  worker/     real Vulkan executor, device/object state, and Win32 windowing

tests/
  unit/       pure and component-level contracts
  integration/ protocol, process, mock, real Vulkan, and Win32 integration
  fuzz/       bounded wire and Vulkan-RPC decoder fuzz loops
  smoke/      shell boundary checks
```

## Build and Gate

The normal dual-platform gate from WSL is:

```bash
cd vkrelay2
./scripts/dev/rebuild_all.sh --all
```

This performs debug builds, Windows and Linux CTest suites, clang-format verification, and `bash -n`
for tracked shell scripts. Use `--clean --all` before a release or after changing toolchain inputs.

Release builds are separate:

```bash
./scripts/dev/rebuild_all.sh --release
```

The launcher prefers release output, so application testing after a debug-only build may still run
the older release tree. The launcher and build helper both warn about this skew.

## Test Layers

The repository intentionally tests boundaries at several levels:

- Unit tests pin JSON, framing, descriptor validation, GPU selectors, RPC decoders, structure
  admission, memory bookkeeping, sidecar registries, and pure window policies.
- Mock integration tests exercise control/data-plane handshakes, worker lifetime, protocol parity,
  process teardown, and failure behavior without a host Vulkan dependency.
- Real Windows integration tests invoke the same real backend used by a worker and verify host
  object creation, drawing, compute, memory, synchronization, descriptors, textures, readback, and
  Win32 extent behavior.
- Linux smoke scripts launch canaries through the real loader, ICD, supervisor, worker, sidecar,
  private compositor, and X server.
- Fuzz tests feed arbitrary bounded inputs to the wire and RPC decoders and assert total decoding.

CTest covers compiled tests. The `linux/launcher/run_*_smoke.sh` scripts are targeted end-to-end
gates and may require a real daemon, display components, and GPU.

See [Testing](testing.md) for the real-application render catalog, aggregate smoke gate, strict
dependency mode, capture artifacts, and focused commands.

## Runtime Tracing

Useful opt-in variables include:

| Variable | Effect |
|---|---|
| `VKRELAY2_ICD_TRACE=1` | Logs ICD calls, local admission, and RPC outcomes. |
| `VKRELAY2_GEOM_TRACE=1` | Worker-side: logs host geometry forwarding. Must be present in the Windows supervisor's environment before it starts. |
| `VKRELAY2_DEBUG_WINDOW_TITLES=1` | Worker-side: tags host window titles with guest XIDs. Must be present in the Windows supervisor's environment before it starts. |
| `VKRELAY2_PROFILE=1` | Enables per-session client and worker RPC profiling. |
| `VKRELAY2_PROFILE_FRAMES=1` | Adds a bounded recent-frame timing ring to profile output. |
| `VKRELAY2_PROFILE_OUT=<path>` | Selects a profile output path where supported. |

The worker inherits ordinary environment variables from the long-lived Windows supervisor. A
variable prefixed to `vkrun` configures the WSL process tree, not a reused or
auto-started Windows daemon. For worker-side variables above, stop the daemon and start the
supervisor from a Windows shell with the worker-side variable set. `VKRELAY2_PROFILE=1` is different: the
launcher carries that request through the control protocol and the supervisor enables profiling on
the new worker for that session.

Profiling emits `VKRELAY2-PROFILE` records containing per-operation counts, bytes, latency
histograms, command-record phase costs, upload-sweep costs, and frame rollups. Summarize a captured
log with:

```bash
./scripts/dev/profile_report.sh <profile-log>
```

Do not enable tracing for performance measurements unless the trace overhead is part of the test.

## Focused Window Capture

There are two capture paths for different questions.

### Source-layer capture

`vkrelay2-capture` connects to a running session's sidecar plane and captures the worker-owned chrome
or cursor BGRA source, producing PNG plus JSON metadata. It is deterministic and does not depend on
desktop visibility, occlusion, or window placement.

The tool accepts either one lifecycle-pinned XID or all enumerated windows:

```text
vkrelay2-capture --connect HOST:PORT --sidecar-token TOKEN \
    --xid XID --expected-generation GENERATION --expected-epoch EPOCH \
    --layer chrome --out PREFIX

vkrelay2-capture --connect HOST:PORT --sidecar-token TOKEN \
    --all --layer chrome --out PREFIX
```

The launcher provides the sidecar endpoint and token only to the application process tree. Capture
automation should run as a child of that environment rather than copying tokens into a global
configuration.

### Composed host-window capture

`scripts/dev/capture_window.ps1` selects a vkrelay2 Win32 window and captures its composed client
area with Windows graphics APIs. This is useful for inspecting the final on-screen result, including
Vulkan content, but Windows may return stale or blank content for minimized or otherwise unavailable
windows. Prefer source-layer capture when testing chrome/cursor fidelity independent of desktop
state.

## Protocol Changes

When extending a protocol:

1. Keep frame and allocation bounds explicit.
2. Validate wire types, field presence, numeric domains, counts, and byte sizes before allocating or
   calling a backend.
3. Keep mock and real backend admission behavior aligned.
4. Treat omitted additive fields as an explicit compatibility state; do not let a transmitted value
   forge omission.
5. Scope handles to their owning instance/device/session and validate destroy order.
6. Add negative unit coverage, mock integration coverage, and a real-host proof for behavior that
   depends on Vulkan or Win32.

The control, application, and sidecar planes have separate handshakes and dispatchers. Do not route a
new sidecar operation through the Vulkan operation space or vice versa.
