# vkrelay2 private-session helper -- SOURCED, not executed.
#
# Brings up a private weston-headless + rootless-Xwayland display, opens a worker session
# through the daemon, and pins the vkrelay2 ICD into the environment -- all FAIL-CLOSED. Shared
# by vkrun's app-run path and run_canary.sh so the "establish the session +
# display + ICD before launching, or fail" logic lives in exactly one place (the normal app-run
# path must not bypass this and launch against the system Vulkan stack).
#
# Caller contract: set up a trap calling `vkr_session_cleanup "$?"` on EXIT, then call
# `vkr_start_private_display` and `vkr_open_session_and_pin_icd <launch_bin> <manifest> <app_id>`
# (each returns non-zero on failure -- the caller must NOT launch the app on failure). The
# functions export DISPLAY / WAYLAND_DISPLAY / XDG_RUNTIME_DIR / VKRELAY2_DATA_PLANE_* /
# VKRELAY2_APP_TOKEN / VK_ICD_FILENAMES / VK_DRIVER_FILES.
#
# Safe ownership: a unique mktemp runtime dir + owned Wayland socket; the Xwayland
# display is taken via Xwayland's own O_EXCL lock (try sequential :N, use the first that starts
# -- never rm a lock that may belong to another process); readiness gated on the
# compositor/X sockets; cleanup tears down only what we spawned and preserves logs on failure.

# Absolute directory of this sourced helper. Keep this independent of the caller's cwd so the
# launcher can find checkout-local runtime dependencies after its private-mount-namespace re-exec.
VKR_PRIVATE_SESSION_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Productization: decide whether a staged private Xwayland was built for the host that is about
# to execute it. The build recipe is deliberately distro-specific; silently running its Jammy
# binary on another distro would trade one known crash for an unsupported ABI/security baseline.
# This helper is pure so the shell gate can exercise every mismatch without launching a display.
vkr_private_xwayland_metadata_compatible() { # <provenance> <codename> <arch> <installed-version> <build-id>
    local provenance="$1" codename="$2" arch="$3" installed="$4" actual_build_id="$5"
    [ -r "${provenance}" ] || return 1
    local target source_version expected_build_id freshness
    target="$(awk '$1 == "target:" { print $2; exit }' "${provenance}")"
    source_version="$(awk '$1 == "source_package:" { print $2; exit }' "${provenance}")"
    expected_build_id="$(awk '$1 == "build_id:" { print $2; exit }' "${provenance}")"
    freshness="$(awk '$1 == "freshness_verified:" { print $2; exit }' "${provenance}")"
    [ -n "${codename}" ] && [ -n "${arch}" ] && [ "${target}" = "${codename}/${arch}" ] || return 1
    [ -n "${installed}" ] && [ "${source_version}" = "${installed}" ] || return 1
    [ -n "${actual_build_id}" ] && [ "${expected_build_id}" = "${actual_build_id}" ] || return 1
    [ "${freshness}" = "yes" ] || return 1
}

# The supported distros whose STOCK Xwayland is empirically confirmed to NULL-deref on this seatless
# weston-headless compositor (xwl_cursor_warped_to at 0xa0 on every version; damage_report at 0x40 on
# Xwayland 24.1.x). Stock upstream has not merged vkrelay2's guards, so every stock build on these
# distros is unsafe and the guarded private build is required. Kept to the TESTED set (not "any
# distro") so an untested distro is not blindly refused; an explicit VKRELAY2_XWAYLAND_BIN always
# lets a developer run a stock control. Args 3/4 (version/build-id) are unused now that the whole
# tested distro is known-unsafe, but the signature is kept for callers.
vkr_stock_xwayland_known_unsafe() { # <codename> <arch> <installed-version> <build-id>
    [ "$2" = "amd64" ] || return 1
    case "$1" in
    jammy | noble | resolute) return 0 ;;
    esac
    return 1
}

# Print the compatible package-installed or checkout-local private Xwayland, or nothing. Explicit
# VKRELAY2_XWAYLAND_BIN remains authoritative; this is only the no-override convenience path.
vkr_find_compatible_private_xwayland() {
    command -v readelf >/dev/null 2>&1 || return 1

    local codename="" arch="" installed="" actual_build_id=""
    if [ -r /etc/os-release ]; then
        codename="$(. /etc/os-release; printf '%s' "${VERSION_CODENAME:-}")"
    fi
    if command -v dpkg >/dev/null 2>&1; then
        arch="$(dpkg --print-architecture 2>/dev/null || true)"
    fi
    if command -v dpkg-query >/dev/null 2>&1; then
        installed="$(dpkg-query -W -f='${Version}' xwayland 2>/dev/null || true)"
    fi

    local candidate provenance
    for candidate in \
        "${VKR_PRIVATE_SESSION_LIB_DIR}/../xwayland/Xwayland" \
        "${VKR_PRIVATE_SESSION_LIB_DIR}/../../build/src_ext/xwayland/stage/Xwayland"; do
        provenance="$(dirname "${candidate}")/PROVENANCE.txt"
        [ -x "${candidate}" ] && [ -r "${provenance}" ] || continue
        actual_build_id="$(readelf -n "${candidate}" 2>/dev/null \
            | awk '/Build ID:/ { print $3; exit }')"
        if vkr_private_xwayland_metadata_compatible \
            "${provenance}" "${codename}" "${arch}" "${installed}" "${actual_build_id}"; then
            readlink -f "${candidate}"
            return 0
        fi
    done
    return 1
}

# --- WSLg read-only /tmp/.X11-unix workaround (transparent, no sudo, no global change) ----------
# Newer WSLg bind-mounts /tmp/.X11-unix READ-ONLY (to protect its own :0 socket). The X11 socket
# directory is hardcoded by the X transport, so our private Xwayland cannot create its socket there
# and the private-display bring-up fails with an opaque "could not start Xwayland on any display".
# Rather than ask the user to disable WSLg distro-wide (guiApplications=false) or sudo-remount a
# system path -- both global and intrusive -- a caller re-execs itself inside a PRIVATE user+mount
# namespace and overlays a writable tmpfs on /tmp/.X11-unix. That overlay is visible ONLY to this
# process tree and reverts when it exits; WSLg and every other app are untouched; no root is needed
# (an unprivileged user namespace maps our uid to root inside the namespace just far enough to mount
# a tmpfs there). Callers invoke `vkr_reexec_in_private_x11_namespace "$@"` BEFORE
# vkr_start_private_display (and only when they actually need the private display).

# Is /tmp/.X11-unix already usable for a private X server (exists + sticky + writable, or creatable)?
vkr_x11_dir_usable() {
    local d=/tmp/.X11-unix
    if [ -d "${d}" ]; then
        [ -k "${d}" ] || return 1 # Xwayland requires mode 1777 (sticky)
        local probe="${d}/.vkrelay2-wtest.$$"
        if ( : >"${probe}" ) 2>/dev/null; then rm -f "${probe}"; return 0; fi
        return 1 # exists but not writable (the WSLg read-only mount)
    fi
    [ -w /tmp ] # missing: fine as long as the X server can create it 1777
}

# True only when the namespace re-exec restored the exact numeric caller identity captured before
# the outer root mapping. Kept pure so the shell smoke can exercise malformed/missing markers without
# entering a namespace.
vkr_x11ns_identity_matches() { # <expected-uid> <expected-gid>
    local expected_uid="${1:-}" expected_gid="${2:-}" actual_uid actual_gid
    case "${expected_uid}" in '' | *[!0-9]*) return 1 ;; esac
    case "${expected_gid}" in '' | *[!0-9]*) return 1 ;; esac
    actual_uid="$(id -u 2>/dev/null)" || return 1
    actual_gid="$(id -g 2>/dev/null)" || return 1
    [ "${actual_uid}" = "${expected_uid}" ] && [ "${actual_gid}" = "${expected_gid}" ]
}

# Exercise the complete privilege transition in a disposable directory: acquire mount privilege in
# the outer root-mapped user+mount namespace, create a private tmpfs, then enter the nested user
# namespace mapped back to the caller and prove that identity + tmpfs write access both hold. This
# is an execution probe -- never infer support from util-linux help text (its spelling differs across
# Jammy/Noble/Resolute). The mount disappears with the probe namespace; only the empty underlying
# directory exists in the caller and is removed before return.
vkr_probe_identity_preserving_x11_namespace() { # <caller-uid> <caller-gid>
    local caller_uid="${1:-}" caller_gid="${2:-}" probe_dir probe_ok=0
    case "${caller_uid}" in '' | *[!0-9]*) return 1 ;; esac
    case "${caller_gid}" in '' | *[!0-9]*) return 1 ;; esac
    command -v unshare >/dev/null 2>&1 || return 1
    command -v mount >/dev/null 2>&1 || return 1
    probe_dir="$(mktemp -d "${TMPDIR:-/tmp}/vkrelay2-x11ns-probe.XXXXXX" 2>/dev/null)" \
        || return 1

    local inner_script outer_script
    inner_script='
        [ "$(id -u 2>/dev/null)" = "$1" ] || exit 20
        [ "$(id -g 2>/dev/null)" = "$2" ] || exit 21
        : > "$3/.identity-write-ok" || exit 22
    '
    outer_script='
        mount -t tmpfs tmpfs "$1" || exit 10
        chmod 1777 "$1" || exit 11
        exec unshare --user --map-user="$2" --map-group="$3" -- \
            bash -c "$4" _ "$2" "$3" "$1"
    '
    if unshare --user --map-root-user --mount -- \
        bash -c "${outer_script}" _ "${probe_dir}" "${caller_uid}" "${caller_gid}" \
        "${inner_script}" >/dev/null 2>&1; then
        probe_ok=1
    fi
    rmdir "${probe_dir}" 2>/dev/null || true
    [ "${probe_ok}" -eq 1 ]
}

vkr_x11ns_identity_failure() {
    echo "vkrelay2: /tmp/.X11-unix is read-only (WSLg mounts it so), but an" >&2
    echo "  identity-preserving private user/mount namespace could not be established." >&2
    echo "  Refusing to launch the application with a different uid. Use ONE of" >&2
    echo "  these remedies, then re-run:" >&2
    echo "    sudo mount -o remount,rw /tmp/.X11-unix && sudo chmod 1777 /tmp/.X11-unix" >&2
    echo "    # or set 'guiApplications=false' under [wsl2] in %USERPROFILE%\\.wslconfig + 'wsl --shutdown'" >&2
    return 1
}

# Re-exec the CALLING script inside a private mount namespace with a writable /tmp/.X11-unix -- ONCE,
# only when needed and only when the caller's numeric uid/gid can be restored before application
# bring-up. The caller passes its "$@" so argv rides across both execs as an array, never a re-parsed
# string. Failure is explicit: callers must not continue to daemon contact after a nonzero return.
vkr_reexec_in_private_x11_namespace() {
    if [ "${VKRELAY2_X11NS:-0}" = "1" ]; then
        if ! vkr_x11ns_identity_matches "${VKR_X11NS_EXPECT_UID:-}" \
            "${VKR_X11NS_EXPECT_GID:-}"; then
            echo "vkrelay2: private X11 namespace did not preserve the invoking uid/gid;" >&2
            echo "  refusing to contact the daemon or launch the application." >&2
            return 1
        fi
        if ! vkr_x11_dir_usable; then
            echo "vkrelay2: private X11 namespace marker is set, but its writable" >&2
            echo "  /tmp/.X11-unix overlay is missing; refusing to continue." >&2
            return 1
        fi
        return 0 # already inside the identity-restored nested namespace
    fi
    vkr_x11_dir_usable && return 0 # /tmp/.X11-unix already works -> nothing to do

    local orig_uid orig_gid
    orig_uid="$(id -u 2>/dev/null)" || { vkr_x11ns_identity_failure; return 1; }
    orig_gid="$(id -g 2>/dev/null)" || { vkr_x11ns_identity_failure; return 1; }
    if ! vkr_probe_identity_preserving_x11_namespace "${orig_uid}" "${orig_gid}"; then
        vkr_x11ns_identity_failure
        return 1
    fi

    echo "vkrelay2: /tmp/.X11-unix is read-only (WSLg); re-running inside a private mount namespace" \
         "with a writable overlay and preserved uid/gid (no sudo, no global change)." >&2
    export VKRELAY2_X11NS=1
    VKR_X11NS_EXPECT_UID="${orig_uid}"
    VKR_X11NS_EXPECT_GID="${orig_gid}"
    export VKR_X11NS_EXPECT_UID VKR_X11NS_EXPECT_GID
    # The outer namespace exists only to mount/chmod the overlay. Before re-running the caller, enter
    # a nested user namespace that maps outer uid/gid 0 back to the original numeric identity. The
    # inner process retains the mount namespace but has no capabilities in its owning (outer) user
    # namespace. Jammy 2.37.2, Noble 2.39.3, and Resolute 2.41.3 all execute this exact option shape.
    exec unshare --user --map-root-user --mount -- bash -c '
        mount -t tmpfs tmpfs /tmp/.X11-unix || exit 1
        chmod 1777 /tmp/.X11-unix || exit 1
        uid="$1"; gid="$2"; shift 2
        exec unshare --user --map-user="${uid}" --map-group="${gid}" -- "$@"
    ' _ "${orig_uid}" "${orig_gid}" bash "$0" "$@"
}

# launcher/session reliability: phase-stamped bring-up progress so a hang can be
# LOCALIZED to a phase. Each milestone is written to stderr (live) AND, once a phase log exists,
# appended to it; vkr_session_cleanup folds that log into the failure bundle. VKR_PHASE_LOG is created
# by the app-run path (vkrun); when it is unset (e.g. --list-gpus) this is
# stderr-only. Elapsed seconds come from bash's SECONDS (since this shell started), enough to see
# which phase stalled.
vkr_phase() {
    local line="vkrelay2: [phase +${SECONDS:-0}s] $1"
    echo "${line}" >&2
    if [ -n "${VKR_PHASE_LOG:-}" ]; then
        printf '%s\n' "${line}" >>"${VKR_PHASE_LOG}" 2>/dev/null || true
    fi
}

vkr_session_cleanup() {
    local ec="${1:-0}"
    # Idempotent: a signal trap (INT/TERM, e.g. `timeout`) runs cleanup then the EXIT trap fires too;
    # only the first invocation preserves + tears down.
    [ -n "${VKR_CLEANED:-}" ] && return 0
    VKR_CLEANED=1
    # The wrapper owns the target app process group, so its EXIT/INT/TERM trap is the authoritative
    # app-lifetime edge. Pure-X11 apps never load our Vulkan ICD and therefore never open/close the
    # worker data plane; explicitly close their authenticated worker session before tearing down
    # the display. SessionClosed is an actual process-death acknowledgement, so returning from this
    # bounded call also proves Windows has reclaimed the placeholder HWND. Best-effort in cleanup:
    # preserve the original app exit code and continue reclaiming Linux children if the daemon is
    # already gone.
    if [ -n "${VKR_SESSION_LAUNCH_BIN:-}" ] && [ -n "${VKRELAY2_WORKER_ID:-}" ] \
        && [ -n "${VKRELAY2_APP_TOKEN:-}" ]; then
        vkr_phase "cleanup: closing authenticated worker session"
        timeout "${VKRELAY2_CLOSE_SESSION_TIMEOUT:-8}" "${VKR_SESSION_LAUNCH_BIN}" \
            --close-session --app-id "${VKR_SESSION_APP_ID:-vkrelay2-app}" \
            --worker-id "${VKRELAY2_WORKER_ID}" --app-token "${VKRELAY2_APP_TOKEN}" \
            >/dev/null 2>&1 \
            || echo "vkrelay2: warning: worker session close was not acknowledged" >&2
    fi
    [ -n "${VKR_SIDECAR_PID:-}" ] && kill "${VKR_SIDECAR_PID}" 2>/dev/null || true
    [ -n "${VKR_XWAYLAND_PID:-}" ] && kill "${VKR_XWAYLAND_PID}" 2>/dev/null || true
    [ -n "${VKR_WESTON_PID:-}" ] && kill "${VKR_WESTON_PID}" 2>/dev/null || true
    if [ -n "${VKR_RUNTIME_DIR:-}" ] && [ -d "${VKR_RUNTIME_DIR}" ]; then
        # Preserve the FULL session log bundle on a failing/timeout exit (or always, when the caller
        # sets VKRELAY2_LOG_DIR) so an OpenSCAD/GL crash is debuggable. The prior version
        # copied only *.log and left an empty/insufficient bundle, so the last crash could not be
        # chased. Now: copy the whole runtime dir (sidecar.log, app.log, weston/xwayland logs, the
        # daemon.port, ...) PLUS the Windows-side worker log if we redirected one, and ANNOUNCE the
        # bundle + its contents so the path is never guessed.
        if [ "${ec}" != "0" ] || [ -n "${VKRELAY2_LOG_DIR:-}" ]; then
            local keep="${VKRELAY2_LOG_DIR:-/tmp/vkrelay2-logs-$$}"
            if mkdir -p "${keep}" 2>/dev/null; then
                cp -r "${VKR_RUNTIME_DIR}"/. "${keep}/" 2>/dev/null || true
                # The worker runs on the Windows side; pull its redirected log in if present.
                if [ -n "${VKR_WORKER_LOG_WSL:-}" ] && [ -f "${VKR_WORKER_LOG_WSL}" ]; then
                    cp "${VKR_WORKER_LOG_WSL}" "${keep}/worker.log" 2>/dev/null || true
                fi
                # the phase trace lives outside the runtime dir (it starts before that dir exists,
                # at daemon reachability), so fold it in too -- it shows which bring-up phase stalled.
                if [ -n "${VKR_PHASE_LOG:-}" ] && [ -f "${VKR_PHASE_LOG}" ]; then
                    cp "${VKR_PHASE_LOG}" "${keep}/phase.log" 2>/dev/null || true
                fi
                echo "vkrelay2: ===== session logs preserved in ${keep} (exit ${ec}) =====" >&2
                ls -1 "${keep}" 2>/dev/null | sed 's/^/vkrelay2:   /' >&2 || true
            fi
        fi
        # Remove only state we created; never touch /tmp/.X*-lock (Xwayland owns/cleans its own).
        rm -rf "${VKR_RUNTIME_DIR}" 2>/dev/null || true
    elif [ -n "${VKR_PHASE_LOG:-}" ] && [ -f "${VKR_PHASE_LOG}" ] \
        && { [ "${ec}" != "0" ] || [ -n "${VKRELAY2_LOG_DIR:-}" ]; }; then
        # PRE-display failure: app-run can fail fast BEFORE the runtime dir exists --
        # e.g. ensure_daemon aborts on a wedged/absent daemon. There are no worker/display/app logs
        # yet, but the phase trace shows how far bring-up got, so preserve a PHASE-ONLY bundle rather
        # than silently dropping it -- keeping the VKRELAY2_LOG_DIR / nonzero-exit promise honest.
        local keep="${VKRELAY2_LOG_DIR:-/tmp/vkrelay2-logs-$$}"
        if mkdir -p "${keep}" 2>/dev/null; then
            cp "${VKR_PHASE_LOG}" "${keep}/phase.log" 2>/dev/null || true
            echo "vkrelay2: ===== phase trace preserved in ${keep} (exit ${ec}; pre-display failure) =====" >&2
        fi
    fi
    # The phase trace is a standalone temp file (created before the runtime dir); it has been folded
    # into the bundle above when needed, so drop the original.
    [ -n "${VKR_PHASE_LOG:-}" ] && rm -f "${VKR_PHASE_LOG}" 2>/dev/null || true
}

vkr_start_private_display() {
    command -v weston >/dev/null 2>&1 || { echo "vkrelay2: weston not found" >&2; return 1; }

    # fix selection: an explicit VKRELAY2_XWAYLAND_BIN wins. Without one, auto-select the
    # checkout-local guarded build only when its provenance exactly matches the running distro,
    # architecture, installed Xwayland package, and ELF build ID. Otherwise use the system server.
    # This closes the footgun where a normal `-- blender` invocation silently selected Jammy's
    # known-crashing stock 22.1.1 despite a compatible guarded stage being present, while preserving
    # the recipe's fail-closed rule against importing the Jammy binary onto another distro.
    # Resolve + identity-log BEFORE launch so a failure bundle proves exactly which server ran.
    local xwayland_bin="${VKRELAY2_XWAYLAND_BIN:-}"
    if [ -n "${xwayland_bin}" ]; then
        [ -x "${xwayland_bin}" ] \
            || { echo "vkrelay2: VKRELAY2_XWAYLAND_BIN not executable: ${xwayland_bin}" >&2; return 1; }
    elif xwayland_bin="$(vkr_find_compatible_private_xwayland)"; then
        echo "vkrelay2: auto-selected compatible private Xwayland stage" >&2
    else
        xwayland_bin="$(command -v Xwayland 2>/dev/null)" \
            || { echo "vkrelay2: Xwayland not found" >&2; return 1; }
        # Do not silently launch the exact stock server already proven to crash on this seatless
        # compositor. A clear dependency error is far better than killing every X client later.
        local host_codename="" host_arch="" host_xwl_version="" host_xwl_build_id=""
        if [ -r /etc/os-release ]; then
            host_codename="$(. /etc/os-release; printf '%s' "${VERSION_CODENAME:-}")"
        fi
        host_arch="$(dpkg --print-architecture 2>/dev/null || true)"
        host_xwl_version="$(dpkg-query -W -f='${Version}' xwayland 2>/dev/null || true)"
        if command -v readelf >/dev/null 2>&1; then
            host_xwl_build_id="$(readelf -n "${xwayland_bin}" 2>/dev/null \
                | awk '/Build ID:/ { print $3; exit }')"
        fi
        if vkr_stock_xwayland_known_unsafe \
            "${host_codename}" "${host_arch}" "${host_xwl_version}" "${host_xwl_build_id}"; then
            echo "vkrelay2: refusing known-crashing stock Xwayland ${host_xwl_version}" >&2
            echo "  A guarded/tested Xwayland is required for application runs." >&2
            if [ -r "${VKR_PRIVATE_SESSION_LIB_DIR}/../windows-payload/VERSION" ]; then
                echo "  This installed package's guarded Xwayland no longer matches the host" >&2
                echo "  xwayland security package. Install a refreshed vkrelay2 .deb for this" >&2
                echo "  Ubuntu release, then retry." >&2
            else
                echo "  Build the guarded dependency with:" >&2
                echo "    bash ${VKR_PRIVATE_SESSION_LIB_DIR}/../../src_ext/xwayland/build_private_xwayland.sh" >&2
                echo "  This is a one-time stage, not part of each vkrelay2 rebuild." >&2
            fi
            echo "  Developers may instead explicitly set VKRELAY2_XWAYLAND_BIN to a tested server." >&2
            return 1
        fi
    fi
    VKR_XWAYLAND_BIN="${xwayland_bin}"
    echo "vkrelay2: Xwayland binary: ${xwayland_bin}" >&2
    "${xwayland_bin}" -version 2>&1 | head -1 | sed 's/^/vkrelay2: Xwayland version: /' >&2 || true
    if command -v readelf >/dev/null 2>&1; then
        readelf -n "${xwayland_bin}" 2>/dev/null | grep -i "build id" \
            | sed 's/^[[:space:]]*/vkrelay2: Xwayland /' >&2 || true
    fi

    VKR_RUNTIME_DIR="$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/vkrelay2-XXXXXX")" || return 1
    chmod 700 "${VKR_RUNTIME_DIR}"
    export XDG_RUNTIME_DIR="${VKR_RUNTIME_DIR}"
    local wl_sock="vkrelay2-wl-$$"

    # GD (guest display geometry): size the guest root to the HOST's addressable window space -- the
    # pointer-injection warp is absolute-in-root and the X server CLAMPS the pointer to the root, so
    # an undersized root makes every window region beyond it unclickable (and a host-monitor-sized
    # app window dwarfs the 1024x640 weston default, the fragile-resize crash shape). NO hardcoded
    # size: (1) an explicit VKRELAY2_GUEST_ROOT=WxH override (tests / power users) wins; else (2) the
    # pinned virtual-desktop bounds advertised by; else (3) the legacy primary work area; else
    # (4) weston's default. The worker later requires SidecarReady's observed root to exactly match
    # the pinned snapshot, so an override mismatch fails closed before the app launches.
    local gd_w="" gd_h=""
    if [ -n "${VKRELAY2_GUEST_ROOT:-}" ]; then
        gd_w="${VKRELAY2_GUEST_ROOT%%x*}"
        gd_h="${VKRELAY2_GUEST_ROOT##*x}"
    elif [ -n "${VKRELAY2_HOST_VIRTUAL_W:-}" ] && [ -n "${VKRELAY2_HOST_VIRTUAL_H:-}" ]; then
        gd_w="${VKRELAY2_HOST_VIRTUAL_W}"
        gd_h="${VKRELAY2_HOST_VIRTUAL_H}"
    elif [ -n "${VKRELAY2_HOST_WORK_W:-}" ] && [ -n "${VKRELAY2_HOST_WORK_H:-}" ]; then
        gd_w="${VKRELAY2_HOST_WORK_W}"
        gd_h="${VKRELAY2_HOST_WORK_H}"
    fi
    case "${gd_w}" in '' | *[!0-9]*) gd_w="" ;; esac # digits-only, else ignore (fall back)
    case "${gd_h}" in '' | *[!0-9]*) gd_h="" ;; esac
    local size_args=()
    if [ -n "${gd_w}" ] && [ -n "${gd_h}" ] && [ "${gd_w}" -gt 0 ] && [ "${gd_h}" -gt 0 ]; then
        size_args=(--width="${gd_w}" --height="${gd_h}")
        echo "vkrelay2: guest root sized to ${gd_w}x${gd_h} (virtual desktop / legacy / override)" >&2
    fi

    weston --backend=headless-backend.so --socket="${wl_sock}" --idle-time=0 \
        ${size_args[@]+"${size_args[@]}"} \
        >"${VKR_RUNTIME_DIR}/weston.log" 2>&1 &
    VKR_WESTON_PID=$!
    local i
    for i in $(seq 1 50); do
        [ -S "${VKR_RUNTIME_DIR}/${wl_sock}" ] && break
        kill -0 "${VKR_WESTON_PID}" 2>/dev/null \
            || { echo "vkrelay2: weston exited early (see weston.log)" >&2; return 1; }
        sleep 0.1
    done
    [ -S "${VKR_RUNTIME_DIR}/${wl_sock}" ] \
        || { echo "vkrelay2: weston Wayland socket did not appear" >&2; return 1; }
    export WAYLAND_DISPLAY="${wl_sock}"

    # Unsupported internal diagnostic seam (not part of the stable interface; not user-documented; may
    # change or be removed without notice): extra Xwayland args from the environment, e.g.
    # VKRELAY2_XWAYLAND_EXTRA_ARGS="-dumbSched". Split on whitespace WITHOUT pathname/glob expansion
    # (read -r -a, not `(${VAR})`). Empty (the normal case) = default behavior.
    local xwl_extra=()
    if [ -n "${VKRELAY2_XWAYLAND_EXTRA_ARGS:-}" ]; then
        read -r -a xwl_extra <<< "${VKRELAY2_XWAYLAND_EXTRA_ARGS}"
    fi

    local n ok
    for n in $(seq 30 60); do
        "${xwayland_bin}" -rootless ":${n}" ${xwl_extra[@]+"${xwl_extra[@]}"} \
            >"${VKR_RUNTIME_DIR}/xwayland.log" 2>&1 &
        VKR_XWAYLAND_PID=$!
        ok=""
        for i in $(seq 1 30); do
            [ -S "/tmp/.X11-unix/X${n}" ] && { ok=1; break; }
            kill -0 "${VKR_XWAYLAND_PID}" 2>/dev/null || break
            sleep 0.1
        done
        if [ -n "${ok}" ] && kill -0 "${VKR_XWAYLAND_PID}" 2>/dev/null; then
            export DISPLAY=":${n}"
            echo "vkrelay2: private display ready (DISPLAY=:${n}, WAYLAND_DISPLAY=${wl_sock})" >&2
            vkr_phase "private-display: ready (DISPLAY=:${n})"
            return 0
        fi
        kill "${VKR_XWAYLAND_PID}" 2>/dev/null || true
        VKR_XWAYLAND_PID=""
    done
    echo "vkrelay2: could not start Xwayland on any display :30-:60" >&2
    return 1
}

# (pure, testable): echo the GL-frontend env assignments (KEY=VALUE, one per line) to export for
# the given session flags, or NOTHING when the run opts out of relay GL via `--frontend vulkan13`.
# GL-on-zink is the DEFAULT -- auto / opengl46zink / unspecified all enable it, matching the original
# relay's wrapper, so OpenGL apps need no flag. No side effects; tests/smoke/gl_frontend_env_bash.sh
# asserts this decision on both platforms. NOTE: the `vulkan13` opt-out REFRAINS from
# exporting the four GL vars; it does NOT actively unset a caller-supplied parent env, so a parent that
# already set e.g. GALLIUM_DRIVER=zink still wins. Fine for the current contract; if `vulkan13` ever
# means a STRICT clean GL env, unset the four vars here.
vkr_gl_frontend_env() {
    case " $* " in
    *" vulkan13 "*)
        return 0 # Vulkan-clean opt-out: no GL env (leave GL on the system stack).
        ;;
    esac
    printf '%s\n' \
        "MESA_LOADER_DRIVER_OVERRIDE=zink" \
        "__GLX_VENDOR_LIBRARY_NAME=mesa" \
        "GALLIUM_DRIVER=zink" \
        "LIBGL_KOPPER_DRI2=1"
}

# (guest Mesa probe): print the guest's Mesa MAJOR version, or nothing when unknown. Zink's
# minimum Vulkan surface is a MESA property (23.x runs on a 1.2 device; >= 24 requires a 1.3
# physical device and silently rejects 1.2 ones at pdev selection), so the lane decision below
# needs this one number. The version string is embedded in the zink DRI driver itself -- no GL
# context, no X, no session needed. VKRELAY2_MESA_MAJOR overrides the probe (tests pin it; a user
# with an exotic Mesa layout can too).
vkr_guest_mesa_major() {
    if [ -n "${VKRELAY2_MESA_MAJOR:-}" ]; then
        printf '%s\n' "${VKRELAY2_MESA_MAJOR}"
        return 0
    fi
    # Mesa >= 24.1 unified layout: the version is in the gallium mega-lib's FILENAME
    # (libgallium-25.2.8-...so); zink_dri.so is only a symlink to the tiny dril dispatch stub.
    local lib
    for lib in /usr/lib/*/libgallium-[0-9]*.so* /usr/lib/libgallium-[0-9]*.so*; do
        [ -e "${lib}" ] || continue
        local base="${lib##*/libgallium-}"
        printf '%s\n' "${base%%.*}"
        return 0
    done
    # Mesa <= 23 layout: zink_dri.so IS the driver and embeds its "Mesa NN.N" version string.
    local so
    for so in /usr/lib/*/dri/zink_dri.so /usr/lib/dri/zink_dri.so; do
        [ -r "${so}" ] || continue
        local ver
        ver="$(grep -m1 -aoE 'Mesa [0-9]+\.[0-9]+' "${so}" 2>/dev/null | head -1)"
        if [ -n "${ver}" ]; then
            printf '%s\n' "${ver#Mesa }" | cut -d. -f1
            return 0
        fi
    done
    return 0 # unknown -> caller treats as legacy (the conservative 1.2 cap)
}

# (the Vulkan-1.3 opener -- deterministic, testable): echo the NATIVE-LANE marker for the given
# session flags. `--frontend vulkan13` (a native Vulkan app) always emits `VKRELAY2_NATIVE_LANE=1`,
# which lets the ICD expose the 1.3-family surfaces. The GL lanes (default / auto / opengl46zink)
# emit a MESA-VERSION-DERIVED value: 0 on Mesa <= 23 (zink 23.x is calibrated for the zink-safe
# 1.2 surface -- the proven jammy configuration, unchanged), 1 on Mesa >= 24 (newer zink REQUIRES a
# Vulkan 1.3 physical device and silently rejects a 1.2 one at pdev selection -- the Ubuntu
# 24.04/26.04 finding). Either way the value is emitted UNCONDITIONALLY -- an ACTIVE override, not
# merely "unset" -- so a contaminated parent shell can never steer a run's lane from the outside:
# the lane is launcher-owned and derived only from the frontend flag + the guest Mesa version
# (VKRELAY2_MESA_MAJOR pins the probe for tests/exotic layouts). Asserted on both platforms in
# tests/smoke/gl_frontend_env_bash.sh.
vkr_api_version_env() {
    case " $* " in
    *" vulkan13 "*)
        printf '%s\n' "VKRELAY2_NATIVE_LANE=1"
        ;;
    *)
        local mesa_major
        mesa_major="$(vkr_guest_mesa_major)"
        if [ -n "${mesa_major}" ] && [ "${mesa_major}" -ge 24 ] 2>/dev/null; then
            printf '%s\n' "VKRELAY2_NATIVE_LANE=1"
        else
            printf '%s\n' "VKRELAY2_NATIVE_LANE=0"
        fi
        ;;
    esac
}

vkr_open_session_and_pin_icd() {
    local launch="$1" manifest="$2" app_id="$3"
    shift 3
    # Any remaining args are session-affecting launcher flags (--gpu / --display / --frontend)
    # forwarded verbatim to --open-session, so the launched worker is selected with the user's
    # choice (matching Part 1's faithful selection) instead of the default auto.
    [ -x "${launch}" ] || { echo "vkrelay2: launch binary missing: ${launch}" >&2; return 1; }
    [ -e "${manifest}" ] || { echo "vkrelay2: ICD manifest missing: ${manifest}" >&2; return 1; }

    # --require-worker-backend real makes --open-session fail closed BEFORE it sends LaunchSession
    # if the daemon launches mock workers, so a rejected app-run never spawns an idle worker session.
    #
    # launcher/session reliability: bound the --open-session call with a hard
    # `timeout` BACKSTOP so a wedged daemon (or a hung launch process) can never hang the wrapper
    # forever -- it fails closed and the cleanup trap preserves the log bundle. This is belt-and-braces
    # on top of two faster guards: ensure_daemon's protocol-level --ping health check (~3s) catches a
    # wedged daemon before we get here, and the C++ open_session sets a per-read deadline
    # (VKRELAY2_CONTROL_TIMEOUT_MS, 15s) so a stall typically fails as a clean rc=2 well before this
    # backstop. Default 60s (generous, since the real fast-fail is the ping + the C++ deadline);
    # raise VKRELAY2_OPEN_SESSION_TIMEOUT if a legitimately slow worker spawn needs longer.
    vkr_phase "open-session: contacting daemon (require real worker)"
    local to_s="${VKRELAY2_OPEN_SESSION_TIMEOUT:-60}"
    local session_env rc
    session_env="$(timeout "${to_s}" "${launch}" --open-session --app-id "${app_id}" \
        --require-worker-backend real "$@")" && rc=0 || rc=$?
    if [ "${rc:-1}" -ne 0 ]; then
        if [ "${rc}" -eq 124 ]; then
            echo "vkrelay2: --open-session did not complete within ${to_s}s -- the daemon's control" >&2
            echo "  plane appears wedged. Restart the daemon (stop vkrelay2-supervisor.exe on the" >&2
            echo "  Windows side, then re-run), or raise VKRELAY2_OPEN_SESSION_TIMEOUT for a slow" >&2
            echo "  worker spawn." >&2
        else
            echo "vkrelay2: could not open a real worker session (is the daemon up as" >&2
            echo "  '--serve --vulkan-backend real'?)" >&2
        fi
        return 1
    fi
    local line
    while IFS= read -r line; do
        [ -n "${line}" ] && export "${line?}"
    done <<< "${session_env}"
    [ -n "${VKRELAY2_APP_TOKEN:-}" ] && [ -n "${VKRELAY2_WORKER_ID:-}" ] \
        || { echo "vkrelay2: session did not yield a worker id and app token" >&2; return 1; }
    VKR_SESSION_LAUNCH_BIN="${launch}"
    VKR_SESSION_APP_ID="${app_id}"
    export VKR_SESSION_LAUNCH_BIN VKR_SESSION_APP_ID
    vkr_phase "open-session: session established (real worker)"

    # Fail closed on a mock/unknown daemon: an app-run / on-screen present needs a real host-Vulkan
    # worker. The daemon advertises its session-worker backend at the handshake (VKRELAY2_WORKER_
    # BACKEND); anything but "real" (including absent -> "mock" from --open-session) is rejected so we
    # never launch the app against a worker that produces no real window (a false positive). This
    # also covers reusing an already-running mock daemon.
    if [ "${VKRELAY2_WORKER_BACKEND:-mock}" != "real" ]; then
        echo "vkrelay2: the reachable daemon launches '${VKRELAY2_WORKER_BACKEND:-mock}' workers," \
             "not real host Vulkan." >&2
        echo "vkrelay2: an app-run needs a real worker -- restart the daemon with" \
             "'--serve --vulkan-backend real' (or stop the mock one first)." >&2
        return 1
    fi

    # Pin our ICD so the loader selects it; set both env names for loader-version tolerance.
    export VK_ICD_FILENAMES="${manifest}"
    export VK_DRIVER_FILES="${manifest}"
    # Disable the MESA device-select implicit layer: in vkrelay2 the daemon owns GPU selection (the
    # worker exposes the one device we chose), so this user-facing device picker is redundant. The
    # ICD also implements vkGetPhysicalDeviceProperties2 so it is robust if the layer IS active, but
    # turning it off keeps the app's environment clean and deterministic.
    export NODEVICE_SELECT=1

    #  -- GL-on-zink is the DEFAULT for an app-run (no flag needed). An OpenGL app reaches OUR
    # Vulkan ICD only through Mesa's zink (GL -> Vulkan) Gallium driver, so export the zink GL env BY
    # DEFAULT -- an OpenGL app (glxgears, OpenSCAD, any Qt/GTK GL viewport) runs through the relay with
    # NO special flag, matching the original relay's wrapper. The Vulkan ICD selection itself is
    # already pinned above (VK_ICD_FILENAMES/VK_DRIVER_FILES); these only steer Mesa's GL -> Vulkan
    # choice and are inert for a Vulkan-native app (vkcube never loads GL). The ONE opt-out is
    # `--frontend vulkan13`; `--frontend opengl46zink` and the default/`auto` all enable zink, so
    # older invocations keep working. The decision lives in the pure vkr_gl_frontend_env (above),
    # unit-tested in tests/smoke/gl_frontend_env_bash.sh.
    local _gl_kv
    while IFS= read -r _gl_kv; do
        [ -n "${_gl_kv}" ] && export "${_gl_kv?}"
    done <<< "$(vkr_gl_frontend_env "$@")"

    # (the Vulkan-1.3 opener): set the native-lane marker AUTHORITATIVELY (native frontend =
    # 1, every GL lane = 0). Exported unconditionally so a contaminated parent VKRELAY2_NATIVE_LANE
    # is always overridden -- the zink lane can never be uncapped from the outside.
    local _lane_kv
    while IFS= read -r _lane_kv; do
        [ -n "${_lane_kv}" ] && export "${_lane_kv?}"
    done <<< "$(vkr_api_version_env "$@")"

    # GD: guest font-scale parity with the host (WSLg-like). The relay is 1:1 physical px, so on a
    # scaled monitor (e.g. 150% = dpi 144) a Qt app would otherwise render 96-dpi text ~33% smaller
    # than the WSLg-run app. Only when the daemon advertised a DPI and the user has not chosen their
    # own Qt scaling (never override an explicit user choice). Coordinates are unaffected (widget
    # metrics change; the coordinate space stays physical px).
    if [ -n "${VKRELAY2_HOST_DPI:-}" ] && [ -z "${QT_FONT_DPI:-}" ] && [ -z "${QT_SCALE_FACTOR:-}" ]; then
        export QT_FONT_DPI="${VKRELAY2_HOST_DPI}"
    fi
    # GD: a writable mesa shader cache. The private X11 namespace maps our uid to root, so mesa's
    # home-dir cache resolution can land on /root/.cache (Permission denied -> cache disabled + a
    # scary warning). Point it at the session runtime dir instead (never override a user's choice).
    if [ -z "${MESA_SHADER_CACHE_DIR:-}" ] && [ -n "${VKR_RUNTIME_DIR:-}" ]; then
        export MESA_SHADER_CACHE_DIR="${VKR_RUNTIME_DIR}/mesa-cache"
    fi
    return 0
}

# Start the sidecar (the X11 WM / geometry authority) for this session and BLOCK until it reports
# readiness -- it has claimed the WM, probed extensions, done its initial root scan, and the worker
# has acked sidecar_ready. The app MUST NOT launch before this (readiness barrier); the
# barrier is an observable edge (the sidecar writes to an inherited fd AFTER the worker's ack),
# never log scraping. A no-op (success) when the session carries no sidecar plane -- so an older or
# mock daemon, which exposes no VKRELAY2_SIDECAR_*, simply proceeds without a sidecar. Returns
# non-zero on a real failure (a sidecar that could not claim the WM or never became ready), and the
# caller must then NOT launch the app.
vkr_start_sidecar_and_wait_ready() {
    local sidecar="$1"
    shift # remaining args (e.g. --debug-inject for the input smoke) pass through to the sidecar
    # No sidecar plane on this session (older/mock daemon) -> nothing to do, not an error.
    [ -n "${VKRELAY2_SIDECAR_PLANE_PORT:-}" ] && [ -n "${VKRELAY2_SIDECAR_TOKEN:-}" ] || return 0
    [ -x "${sidecar}" ] || { echo "vkrelay2: sidecar binary missing: ${sidecar}" >&2; return 1; }

    local host="${VKRELAY2_SIDECAR_PLANE_HOST:-127.0.0.1}"
    local fifo="${VKR_RUNTIME_DIR:-/tmp}/vkrelay2-sidecar-ready.$$"
    mkfifo "${fifo}" 2>/dev/null \
        || { echo "vkrelay2: could not create the sidecar ready pipe" >&2; return 1; }
    # Open the pipe read-write on fd 3 (a <> open does not block); the sidecar inherits fd 3 and
    # writes one byte to it after the worker acks sidecar_ready. Unlink the name -- the open fd keeps
    # the pipe alive, and the sidecar uses the inherited fd, not the path.
    exec 3<>"${fifo}"
    rm -f "${fifo}" 2>/dev/null || true

    vkr_phase "sidecar: launching + waiting on the readiness barrier"
    "${sidecar}" --connect "${host}:${VKRELAY2_SIDECAR_PLANE_PORT}" \
        --sidecar-token "${VKRELAY2_SIDECAR_TOKEN}" --display "${DISPLAY}" --ready-fd 3 "$@" \
        >"${VKR_RUNTIME_DIR:-/tmp}/sidecar.log" 2>&1 &
    VKR_SIDECAR_PID=$!

    # Block until the sidecar raises the readiness edge (bounded so a stuck sidecar cannot wedge us).
    local ready="" line
    if IFS= read -r -t 15 -u 3 line; then ready=1; fi
    exec 3>&- # close our read end (the app launched next must not inherit it)
    if [ -z "${ready}" ]; then
        echo "vkrelay2: sidecar did not become ready (see sidecar.log)" >&2
        vkr_phase "sidecar: NOT ready (timed out waiting for the readiness edge)"
        kill "${VKR_SIDECAR_PID}" 2>/dev/null || true
        VKR_SIDECAR_PID=""
        return 1
    fi
    echo "vkrelay2: sidecar ready (WM claimed on ${DISPLAY})" >&2
    vkr_phase "sidecar: ready (WM claimed on ${DISPLAY})"
    return 0
}
