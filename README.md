# vkrelay2

vkrelay2 runs Linux graphics applications in WSL2 while executing their Vulkan work on a Windows
Vulkan driver and presenting each application window as a native Win32 window.

It provides two graphics paths:

- native Vulkan applications use the vkrelay2 Vulkan ICD;
- X11 OpenGL applications use Mesa Zink, which translates OpenGL to Vulkan before it reaches the
  same ICD.

The supported user-facing launcher creates an isolated Weston/Xwayland session for each
application, starts or reuses a Windows supervisor, creates a dedicated Windows worker, pins the
relay ICD, and starts the application only after the graphics and window-management paths are
ready. It does not use WSLg window projection for application windows.

## Current Scope

The current implementation targets:

- Windows 11 with WSL2;
- 64-bit Linux applications using X11/Xwayland;
- a Windows Vulkan adapter selected at launch time;
- native Vulkan, including an opt-in Vulkan 1.3 lane when the selected host device and relay
  capabilities support it;
- desktop OpenGL applications through Mesa Zink;
- native Win32 top-level windows, popup/menu windows, input, cursors, move/resize feedback,
  minimize/restore, and captured X11 chrome.
- one physical-pixel canvas spanning mixed-resolution/DPI, negative-origin, and portrait Windows
  monitor layouts, with snapshot-driven per-monitor placement and maximize behavior.

The project can be run from a source checkout or from a binary package produced by
`scripts/dev/package_release.sh` (see [Installing](docs/install.md)); there is no OS-level
installer yet. The ICD exposes only functionality backed by the relay and rejects unsupported
extensions, features, pipeline shapes, and commands. It is not presented as a Vulkan conformant
implementation.

See [Architecture](docs/architecture.md) for the process and data-flow model and
[Current Limitations](docs/usage.md#current-limitations) before evaluating an application.

## Quick Start

### From a binary package (no build tools needed)

Download the package for your WSL Ubuntu release (`jammy`, `noble`, or `resolute`) from the
[latest GitHub Release](https://github.com/dchirila/vkrelay/releases/latest), verify its checksum,
extract it under `/mnt/c`, and run `./install.sh` inside WSL. It installs runtime packages only;
compilers, Visual Studio, and the Vulkan SDK are not required. See [Installing](docs/install.md) for
copy-paste commands.

### From source

Ubuntu 22.04 (jammy), 24.04 (noble), and 26.04 (resolute) amd64 in WSL2 are all validated, with
Visual Studio 2026 on Windows; the full dual-platform gate runs on 22.04. Install the prerequisites
and build both halves as described in [Building](docs/building.md).

From WSL, in the `vkrelay2` source subdirectory:

```bash
./scripts/dev/rebuild_all.sh --release
```

List the Windows Vulkan adapters visible to the supervisor:

```bash
./linux/launcher/vkrun --list-gpus
```

Run an OpenGL application through Zink and the relay. Zink is the default frontend:

```bash
./linux/launcher/vkrun --gpu auto -- glxgears
./linux/launcher/vkrun --gpu high-performance -- openscad
```

Run a native Vulkan application on the Vulkan 1.3 lane:

```bash
./linux/launcher/vkrun \
    --frontend vulkan13 --gpu high-performance -- vkcube
```

Everything after `--` is the target application's argument vector and is forwarded without shell
reconstruction:

```bash
./linux/launcher/vkrun -- \
    openscad /usr/share/openscad/examples/Basics/CSG.scad
```

The launcher prints phase markers during startup. If startup or the application fails, it preserves
a diagnostic bundle under `/tmp/vkrelay2-logs-<pid>` and prints the exact path.

## Documentation

- [Installing](docs/install.md): binary packages — producing one, and the minimal install steps
- [Usage](docs/usage.md): frontends, GPU selection, application launch, networking, and runtime
  configuration
- [Building](docs/building.md): prerequisites, Windows and WSL builds, private Xwayland, tests, and
  linting
- [Architecture](docs/architecture.md): component ownership, protocols, Vulkan forwarding, and
  window integration
- [Vulkan support](docs/vulkan-support.md): API-version policy, implemented families, memory/WSI
  behavior, and unsupported shapes
- [Troubleshooting](docs/troubleshooting.md): startup failures, daemon health, Zink, Xwayland, and
  diagnostic bundles
- [Development](docs/development.md): source layout, test layers, tracing, profiling, and capture
  tools

## Repository Layout

The implementation is under [`vkrelay2/`](vkrelay2/):

```text
vkrelay2/common/               shared protocols, transport, launch, and RPC model
vkrelay2/linux/icd/            Linux Vulkan ICD loaded into applications
vkrelay2/linux/launcher/       supported WSL launcher and private-session setup
vkrelay2/linux/sidecar/        X11 WM, chrome capture, input injection, and geometry bridge
vkrelay2/windows/supervisor/   long-lived control-plane daemon and worker ownership
vkrelay2/windows/worker/       Windows Vulkan backend, RPC executor, and Win32 windows
vkrelay2/tests/                unit, integration, fuzz, and shell smoke tests
vkrelay2/src_ext/xwayland/     pinned private-Xwayland build recipe and local patch
```

## License

vkrelay2 is licensed under the Apache License, Version 2.0 — see [LICENSE](LICENSE) for the full
text and [NOTICE](NOTICE) for attribution. You may use, modify, and distribute it, including in
closed-source and commercial products, provided you retain the copyright and NOTICE attribution and
observe the license's disclaimer of warranty and limitation of liability.

vkrelay2 forwards to a real host Vulkan driver and depends at build and run time on third-party
components (the Vulkan loader, Mesa/Zink, Xwayland, Weston) that carry their own licenses.

## Disclaimer

vkrelay2 is a personal project, developed entirely on my own time and with my own resources. It is not
affiliated with, sponsored, or endorsed by my employer, and nothing in this repository represents the
views or positions of my employer or of any company mentioned.

## Acknowledgements

Although the value proposition of this project is easy to summarize ("make native-feeling OpenGL and
Vulkan work in WSL"), there is a lot of depth underneath. As an after-work project built over two
months of evenings, across two major iterations, it would have been impossible to execute without the
help of my team of AI agents: both Claude Code (Anthropic) and Codex (OpenAI) were used, with agents
taking turns planning, reviewing, and implementing the various phases. Essentially, this was a major
agentic engineering effort, with a human at the steering wheel. That this "agentic symphony" led to
something functional and testable is quite astonishing — living proof of the power of these emerging
tools.

Last, but not least, I would like to express my gratitude to my family and friends, who put up with
my absent-mindedness over the past two months.

## Maturity & future direction

While the software is functional, I would advise caution: consider it a strong prototype
for now. I would be happy to see it grow, but that will take more people getting involved — GPU
vendors and/or Microsoft. Contributions are welcome.

There is still a lot to do (see the current [roadmap](ROADMAP.md)), but more
testing is definitely welcome — due to the dual-OS nature of this project,
there are many possible failure points and wedges to address. Also, I only have
access to a tiny selection of GPUs, so GPU-generation-specific bugs may
surface.
