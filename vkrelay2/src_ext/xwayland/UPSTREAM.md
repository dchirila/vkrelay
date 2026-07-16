# Private Xwayland (vkrelay2 fix)

A maintained, minimal fork carried as **the applicable guard patch(es) on top of the complete Ubuntu
source package** for Xwayland — *not* a pristine-upstream build and *not* a vendored source dump. Only
the artifacts in this directory are tracked; the distro source package is acquired, integrity-checked,
and built on demand into an ignored build directory.

> **Supported targets: Ubuntu 22.04 (jammy), 24.04 (noble), and 26.04 (resolute), amd64.** The recipe
> is distro-adaptive: it detects the host codename/arch and derives the guard set from the Xwayland
> upstream version. It uses one of two acquisition models:
> - **jammy → `acquire: hashpin`** — the reproducible baseline. The source package, its resolved feature
>   outcome, and the amd64 package name are hash-pinned in `sha256sums.txt`; a strict CVE-count and
>   package-test gate applies.
> - **noble / resolute → `acquire: aptsrc`** — the source is fetched with `apt-get source` (integrity
>   from the distribution's signed apt keyring, always the current candidate). The jammy-tuned
>   package-test allowlist does not generalize, so off-jammy the suite is run and recorded under a hard
>   timeout, not fatally gated.
>
> A host outside this set is refused unless `VKRELAY2_XWL_ALLOW_BASELINE_MISMATCH=1`, which attempts the
> `aptsrc` model on the host's own current Xwayland source (accepting that the resolved-config parity
> and guard-offset expectations were calibrated on the supported set). Other override:
> `VKRELAY2_XWL_ALLOW_STALE_BASELINE=1`.

> **Security note.** An earlier revision of this recipe built from *pristine
> upstream* `xwayland-22.1.1.tar.xz`, which silently dropped Ubuntu's entire security-patch series
> (60+ CVE fixes, 2022–2025). That was a security regression — it would have solved one null
> dereference by reintroducing a large set of already-fixed X-server vulnerabilities. Every supported
> path now builds from the **distribution's own security-patched source package** (jammy pins
> `2:22.1.1-1ubuntu0.20`; noble/resolute take the apt candidate), so the full distro CVE stack is
> applied *before* our guards, and the binary is built with Ubuntu's own packaging rules. Do not revert
> any path to a pristine-upstream tarball build.

## Provenance (jammy reproducible baseline — hash-pinned Ubuntu source package)

The table below is the hash-pinned **jammy** baseline. On noble/resolute the `aptsrc` model records the
apt-candidate source-package version and its `apt-get source` input hashes in the stage's
`PROVENANCE.txt` instead; the reproducibility guarantee (pinned hashes) is jammy-only by design.

| | |
|---|---|
| Source package | `xwayland` `2:22.1.1-1ubuntu0.20` (Ubuntu 22.04 **jammy-security**) |
| Pool | http://archive.ubuntu.com/ubuntu/pool/main/x/xwayland/ |
| Inputs (pinned in `sha256sums.txt`) | `…-1ubuntu0.20.dsc`, `…-1ubuntu0.20.debian.tar.xz`, `xwayland_22.1.1.orig.tar.xz` (+`.asc`) |
| Upstream base | Xwayland 22.1.1 — the `.orig.tar.xz` is byte-identical to pristine upstream (`f5d0e0ba…`) |
| Distro delta | the `.debian.tar.xz` quilt series: **60+ CVE patches** (CVE-2022-2319 … CVE-2025-62231) + packaging |
| License | MIT/X11 (X.Org variant) — see `LICENSE` (verbatim upstream `COPYING`) |

The installed stock server on this machine is the same generation; its build ID is
`4b0e1e09c2a5fe4a8c8903a67d8b6aca44303f5a`. Because we build from the identical source package with
the distro packaging rules, **the only behavioural difference from the stock server is our
seatless-warp guard** — every distro security fix is retained. (This statement is now accurate; it
was false for the pristine-upstream revision.)

## The delta — two seatless guards, upstream-submittable

vkrelay2's private compositor is Weston's `headless-backend.so`, which advertises **no `wl_seat`**
(application input is injected out-of-band via XTEST). Stock Xwayland NULL-derefs on two seatless paths;
the recipe applies whichever guards the host's Xwayland version needs, as the **last** entries in the
distro quilt series:

- **`patches/0001-xwayland-guard-seatless-pointer-warp.patch`** — all supported versions.
- **`patches/0002-xwayland-guard-damage-report-null-window.patch`** — Xwayland **24.1.x** (Ubuntu
  26.04) only; 23.2.x and 22.1.x lack the offending line. `damage_report()` guards `xwl_window` for
  NULL in three earlier blocks but derefs `xwl_window->surface_window` unconditionally in the last, so
  a window-move damage report with no associated `xwl_window` crashes at `0x40`. The guard mirrors the
  same-function NULL checks already present. The build derives this set from the upstream version (both
  guards recorded in `PROVENANCE.txt` `guard_patches:`).

The pointer-warp guard (0001) adds a single early return to
`hw/xwayland/xwayland-screen.c::xwl_cursor_warped_to()`:

```c
    if (!xwl_seat)
        xwl_seat = xwl_screen_get_default_seat(xwl_screen);

    if (!xwl_seat)          /* <-- added: mirror xwl_cursor_confined_to() */
        return;
```

**Why:** vkrelay2's private compositor is Weston's `headless-backend.so`, which advertises **no
`wl_seat`** (application input is injected out-of-band via XTEST). `xwl_screen_get_default_seat()`
then returns `NULL`, and the unguarded `xwl_seat->focus_window` dereference below crashes the whole
guest X server (`Segmentation fault at address 0xa0` == `offsetof(struct xwl_seat, focus_window)`)
on **any client pointer warp** — e.g. an application recentering the pointer during a view rotate.
The sibling callback `xwl_cursor_confined_to()` already has exactly this guard; with no seat there
is no Wayland pointer to emulate a warp for, so returning is correct. The core X warp has already
happened; this callback is only the Wayland-side emulation hook. No distro patch touches this
function, so the guard applies cleanly at `--fuzz=0` on top of the full security series.

The same unguarded dereference is present upstream through at least Xwayland 24.1.x, so this is a
genuine upstream defect, not a 22.1.1-only artifact. Keep the patch minimal and submit it upstream
and to Ubuntu; retire this private build for any environment whose packaged Xwayland carries the fix
(and passes `run_rotate_smoke.sh`).

## Reproduction & proof

`vkrelay2-rotate-canary` + `run_rotate_smoke.sh` are a Blender-free, human-free reproducer: a
pure-xcb client that grabs the pointer, `XWarpPointer`-recenters, and ungrabs on the same seatless
weston-headless + rootless-Xwayland stack an app-run uses.

- **Stock** `/usr/bin/Xwayland` (build ID `4b0e1e09…`): `ROTATE-SMOKE: REPRODUCED` — `Segfault at 0xa0`.
- **This build** (security-correct: 60+ CVE patches + our guard): `ROTATE-SMOKE: PASS (SURVIVED)`.

Both runs log the resolved binary path, version, and build ID, so the evidence names the exact
server that ran. Real Blender middle-button view rotation has also been confirmed to work through
the selected private binary.

## Building

Build dependencies (the source package's `Build-Depends`; the set below is jammy's, and is a close
superset on noble/resolute — `VKRELAY2_XWL_INSTALL_DEPS=1` runs `mk-build-deps` on the extracted
`debian/control` for the exact set):

```
dpkg-dev quilt debhelper meson pkg-config
libdrm-dev libepoxy-dev libgcrypt-dev libgbm-dev libnvidia-egl-wayland-dev libpixman-1-dev
libxcvt-dev libxfont-dev libxkbfile-dev libxshmfence-dev libxv-dev libwayland-dev
mesa-common-dev x11proto-dev xfonts-utils xtrans-dev wayland-protocols
```

```sh
bash build_private_xwayland.sh
# - detects host codename/arch; refuses a host outside {jammy,noble,resolute}/amd64 unless
#   VKRELAY2_XWL_ALLOW_BASELINE_MISMATCH=1; verifies security-freshness (see below); overridable
# - acquires the source: jammy hash-verifies the pinned .dsc/.orig/.debian(+.asc) over HTTPS (atomic,
#   re-fetches a bad cache); noble/resolute use `apt-get source` (apt keyring integrity, current candidate)
# - dpkg-source -x -> applies the full distro CVE quilt series (jammy asserts it APPLIED via .pc, incl.
#   the final CVE-2025-62231.patch, and gates a strict CVE count; off-jammy the count is recorded);
#   pins the runtime-affecting `auto` meson options to the stock-resolved values, skipping any option the
#   source dropped (e.g. xwayland_eglstream, gone in 24.1); appends the applicable guard patch(es) as the
#   final quilt entries (asserted; fail on fuzz)
# - builds via dpkg-buildpackage (distro rules + hardening), then runs the package test suite (meson
#   test) under a hard timeout -- re-execing inside a private mount namespace on WSLg so the tests' Xvfb
#   gets a writable /tmp/.X11-unix; jammy gates a small env-only failure allowlist (any other failure is
#   fatal), off-jammy the suite is record-only
# - stages the STRIPPED Xwayland from the built .deb (published atomically), writes stage/PROVENANCE.txt
#   (incl. a release_ready: verdict the packager enforces), asserts no config drift, and prints:
#   VKRELAY2_XWAYLAND_BIN=<abs-path-to-staged-Xwayland>
#
# One-time (needs real root; do it BEFORE a test-enabled build):
#   VKRELAY2_XWL_INSTALL_DEPS=1 bash build_private_xwayland.sh
# This bootstraps devscripts/equivs and uses mk-build-deps; Jammy apt rejects local control/.dsc
# paths passed to apt-get build-dep.
```

Environment knobs:

| var | effect |
|---|---|
| `VKRELAY2_XWL_SKIP_TESTS=1` | skip the package test suite (dev build; manifest records `tests_run: no`) |
| `VKRELAY2_XWL_ALLOWED_TEST_FAILURES` | space-separated allowlist of env-only test failures (default `sync triangles`) |
| `VKRELAY2_XWL_ALLOW_BASELINE_MISMATCH=1` | build the jammy/amd64 baseline on another distro/arch (accept the mismatch) |
| `VKRELAY2_XWL_ALLOW_STALE_BASELINE=1` | build despite an empty/newer distro candidate or a stale apt index (recorded as an override in the manifest) |
| `VKRELAY2_XWL_APT_MAX_AGE_DAYS` | max age of the apt package index for freshness to count as verified (default 7) |
| `VKRELAY2_XWL_ALLOWED_TEST_FAILURES` | `;`-separated **exact** meson test names allowed to fail (default `xwayland / sync;xwayland:xvfb / triangles`) |
| `VKRELAY2_XWL_TEST_GALLIUM_DRIVER` | override the Gallium driver used by package tests; WSL defaults to deterministic `llvmpipe`, while native Linux inherits the host default |
| `VKRELAY2_XWL_BUILD_ROOT` / `VKRELAY2_XWL_WORK_ROOT` | override the staged-output root / the native build work dir |
| `VKRELAY2_XWL_INSTALL_DEPS=1` | install the exact extracted `debian/control` requirements with `mk-build-deps` (host root), then auto re-exec into the normal namespace/test path |

Then select it at launch (the launcher logs the resolved path + version + build ID before launch):

```sh
export VKRELAY2_XWAYLAND_BIN=<abs-path>
VKRELAY2_ROTATE_SMOKE_REQUIRE=1 bash ../../linux/launcher/run_rotate_smoke.sh   # expect PASS
```

Notes:
- **Supported targets are {jammy, noble, resolute}/amd64** (see the box at the top). The recipe detects
  the host and picks the acquisition model (hashpin on jammy, aptsrc otherwise) and guard set; a host
  outside the set is refused unless `VKRELAY2_XWL_ALLOW_BASELINE_MISMATCH=1`. The launcher auto-selects a
  stage only when its provenance `target: <codename>/<arch>` matches the host, so a jammy stage is never
  used on noble/resolute (or vice versa).
- Source/build output go under `vkrelay2/build/src_ext/xwayland/` (git-ignored); on a DrvFs (`/mnt/…`)
  build root the heavy build is redirected to a native work dir (fakeroot/dpkg-deb need POSIX
  ownership) and only the finished stage is copied back. Nothing here overwrites `/usr/bin/Xwayland`.
  The native work dir is namespaced by uid + build-root hash and **`flock`-serialized (mandatory)** so
  concurrent builds don't collide.
- **Atomic publish:** `stage` is a **symlink** to an immutable `gen/<build-id>` directory; each build
  assembles into a fresh generation and repoints the symlink with a single `rename()`, so a launcher
  never sees a missing `stage` or a new binary paired with an old/absent manifest.
- **Security-freshness** is fail-closed: the recipe requires `apt-cache` to be present, a non-empty
  Candidate for `xwayland`, and a recently-refreshed `InRelease` (`≤ VKRELAY2_XWL_APT_MAX_AGE_DAYS`,
  default 7) across any pocket of the host codename (base / `-updates` / `-security`; a dev release may
  carry only the base pocket, a stable one has `-security` — the newest present is gated). On jammy
  (`hashpin`) the Candidate must additionally **equal the pinned version**, so a newer security revision
  forces a pin rebase rather than silently shipping the old one; on `aptsrc` distros `apt-get source`
  always fetches the current candidate, so freshness only has to establish that the index itself is not
  stale. Absent `apt-cache`, an empty candidate, a missing index, or a stale index all fail unless
  `VKRELAY2_XWL_ALLOW_STALE_BASELINE=1` — the sole route to an unverifiable/offline build. The manifest
  records `distro_candidate` and `freshness_verified: yes (…) | no (override/unverifiable)` so an
  override is never mistaken for a verified build.
- `stage/PROVENANCE.txt` records the source-package version, target, `acquire:` model, guard patch set +
  hashes, source input hashes, applied patch stack, resolved feature set, build ID, timestamp,
  `tests_run`/`tests_result`, the freshness verdict, and the `release_ready:` verdict — check it to
  confirm which artifact a `stage/Xwayland` actually is.
- **Feature parity:** the build runs through `debian/rules` (distro meson options + `dpkg-buildflags`
  hardening) and additionally pins the runtime-affecting `auto` options to the stock-resolved values
  (the SHA1 drift — libmd vs libgcrypt, from a stray host `libmd-dev` — showed `auto` leaks the host
  in). The resulting `ldd` and resolved feature defines match `/usr/bin/Xwayland`.
- **Tests:** the package suite runs by default, under a hard `timeout` (`VKRELAY2_XWL_TEST_TIMEOUT`,
  default 420s) so an environment-hung test can never wall-clock the build. On WSLg the recipe re-execs
  inside an unprivileged user+mount namespace (the same trick the launcher uses) so the tests' Xvfb can
  create its socket in a writable `/tmp/.X11-unix`. WSL test runs force Mesa's `llvmpipe` so Xvfb's
  package tests do not accidentally test, or crash inside, the WSLg D3D12/vendor-driver path; the selected
  test renderer is recorded in provenance. On **jammy** the suite is gated: the default allowlist
  covers `sync` (Xvfb socket-listener contention) and `triangles` (rendercheck vs software Render) — both
  run against **Xvfb, not Xwayland**, so they are unaffected by the guard; any failure outside the
  allowlist is fatal. **Off-jammy** the jammy-tuned allowlist does not generalize, so the suite is run and
  its outcome **recorded** (`tests_result`, marked `[record-only on this distro]`) rather than gated;
  off-jammy correctness rests on the distro source + the guards + the resolved-config parity check + the
  application actually rendering. Either way, a stage that skipped tests (`VKRELAY2_XWL_SKIP_TESTS=1`) or
  failed freshness is stamped `release_ready: no` and the packager refuses it.
- The staged binary is the **stripped** server from the built `.deb` (as Ubuntu ships); the matching
  `-dbgsym` `.deb` is left in the work dir if symbols are needed.
- **Build IDs are not deterministic** across build roots/toolchain state; identity logging proves
  *which* binary ran, not reproducibility.
- Offline builds: drop the four pinned source-package files into `dist/` (git-ignored); the hashes
  in `sha256sums.txt` remain authoritative and are checked either way.
