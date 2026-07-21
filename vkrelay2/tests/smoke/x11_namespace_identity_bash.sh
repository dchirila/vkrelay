#!/usr/bin/env bash
# Regression: when WSLg makes /tmp/.X11-unix read-only, the launcher may temporarily map the caller
# to root to mount a private tmpfs, but the application-facing nested namespace must restore the
# caller's numeric uid/gid. The test forces that path, binds a real AF_UNIX socket in the overlay,
# and proves that neither test inode escapes into the host mount namespace.
set -u

here="$(cd "$(dirname "$0")" && pwd)"
lib="${here}/../../linux/launcher/lib_private_session.sh"

fail() {
    echo "X11-NAMESPACE-IDENTITY: FAIL ($*)" >&2
    exit 1
}

if [ ! -f "${lib}" ]; then
    echo "X11-NAMESPACE-IDENTITY: SKIP (lib not found: ${lib})"
    exit 0
fi
# shellcheck source=/dev/null
. "${lib}"

if ! command -v vkr_reexec_in_private_x11_namespace >/dev/null 2>&1 \
    || ! command -v vkr_probe_identity_preserving_x11_namespace >/dev/null 2>&1; then
    fail "namespace helpers not defined after sourcing the lib"
fi

if [ "${1:-}" = "--child" ]; then
    if [ "${VKRELAY2_X11NS:-0}" != "1" ]; then
        # Exercise the production re-exec even when the host's socket directory happens to be
        # writable. The function definition is intentionally not present after the exec.
        vkr_x11_dir_usable() { return 1; }
        vkr_reexec_in_private_x11_namespace "$@" || exit $?
        fail "namespace helper returned instead of re-executing"
    fi

    vkr_reexec_in_private_x11_namespace "$@" \
        || fail "identity/overlay invariant rejected the nested namespace"
    [ "$(id -u)" = "${VKR_TEST_UID}" ] || fail "numeric uid changed inside namespace"
    [ "$(id -g)" = "${VKR_TEST_GID}" ] || fail "numeric gid changed inside namespace"
    [ "$(id -un)" = "${VKR_TEST_USER}" ] || fail "login name changed inside namespace"
    getent passwd "${VKR_TEST_UID}" >/dev/null 2>&1 \
        || fail "passwd lookup for the invoking uid failed"

    read -r uid_real uid_effective uid_saved uid_fs <<EOF
$(awk '/^Uid:/ { print $2, $3, $4, $5 }' /proc/self/status)
EOF
    for observed in "${uid_real}" "${uid_effective}" "${uid_saved}" "${uid_fs}"; do
        [ "${observed}" = "${VKR_TEST_UID}" ] || fail "/proc uid tuple was not restored"
    done
    read -r gid_real gid_effective gid_saved gid_fs <<EOF
$(awk '/^Gid:/ { print $2, $3, $4, $5 }' /proc/self/status)
EOF
    for observed in "${gid_real}" "${gid_effective}" "${gid_saved}" "${gid_fs}"; do
        [ "${observed}" = "${VKR_TEST_GID}" ] || fail "/proc gid tuple was not restored"
    done

    [ -d /tmp/.X11-unix ] && [ -k /tmp/.X11-unix ] && [ -w /tmp/.X11-unix ] \
        || fail "private X11 socket directory is not sticky and writable"
    : >"${VKR_TEST_FILE}" || fail "could not write to private X11 socket directory"
    command -v python3 >/dev/null 2>&1 || fail "python3 is required for the AF_UNIX socket check"
    python3 - "${VKR_TEST_SOCKET}" <<'PY' || fail "could not bind AF_UNIX socket in overlay"
import socket
import sys

sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.bind(sys.argv[1])
sock.listen(1)
sock.close()
PY
    exit 0
fi

unset VKRELAY2_X11NS VKR_X11NS_EXPECT_UID VKR_X11NS_EXPECT_GID
caller_uid="$(id -u)" || fail "could not read caller uid"
caller_gid="$(id -g)" || fail "could not read caller gid"
caller_user="$(id -un)" || fail "could not read caller login name"

vkr_x11ns_identity_matches "${caller_uid}" "${caller_gid}" \
    || fail "identity helper rejected the current numeric identity"
vkr_x11ns_identity_matches "${caller_uid}:0" "${caller_gid}" >/dev/null 2>&1 \
    && fail "identity helper accepted a malformed uid"
vkr_x11ns_identity_matches "${caller_uid}" "${caller_gid}:0" >/dev/null 2>&1 \
    && fail "identity helper accepted a malformed gid"

if (export VKRELAY2_X11NS=1; unset VKR_X11NS_EXPECT_UID VKR_X11NS_EXPECT_GID;
    vkr_reexec_in_private_x11_namespace >/dev/null 2>&1); then
    fail "pre-seeded marker bypassed missing identity metadata"
fi
if (export VKRELAY2_X11NS=1 VKR_X11NS_EXPECT_UID="$((caller_uid + 1))";
    export VKR_X11NS_EXPECT_GID="${caller_gid}";
    vkr_reexec_in_private_x11_namespace >/dev/null 2>&1); then
    fail "pre-seeded marker bypassed mismatched identity metadata"
fi
if (export VKRELAY2_X11NS=1 VKR_X11NS_EXPECT_UID="${caller_uid}";
    export VKR_X11NS_EXPECT_GID="${caller_gid}";
    vkr_x11_dir_usable() { return 1; }
    vkr_reexec_in_private_x11_namespace >/dev/null 2>&1); then
    fail "pre-seeded marker bypassed the missing overlay"
fi
if (vkr_x11_dir_usable() { return 1; }
    vkr_probe_identity_preserving_x11_namespace() { return 1; }
    vkr_reexec_in_private_x11_namespace >/dev/null 2>&1); then
    fail "launcher continued after the full namespace probe failed"
fi

if ! vkr_probe_identity_preserving_x11_namespace "${caller_uid}" "${caller_gid}"; then
    echo "X11-NAMESPACE-IDENTITY: SKIP (kernel/util-linux rejected the full nested namespace probe)"
    exit 0
fi

token="${caller_uid}-$$-${RANDOM:-0}"
export VKR_TEST_UID="${caller_uid}" VKR_TEST_GID="${caller_gid}" VKR_TEST_USER="${caller_user}"
export VKR_TEST_FILE="/tmp/.X11-unix/.vkrelay2-identity-${token}"
export VKR_TEST_SOCKET="/tmp/.X11-unix/.vkrelay2-socket-${token}"
host_had_x0=0
[ -e /tmp/.X11-unix/X0 ] && host_had_x0=1
rm -f "${VKR_TEST_FILE}" "${VKR_TEST_SOCKET}" 2>/dev/null || true

bash "$0" --child || fail "forced production namespace lifecycle failed"
[ ! -e "${VKR_TEST_FILE}" ] || fail "test file escaped the private mount namespace"
[ ! -e "${VKR_TEST_SOCKET}" ] || fail "test socket escaped the private mount namespace"
if [ "${host_had_x0}" -eq 1 ]; then
    [ -e /tmp/.X11-unix/X0 ] || fail "host X0 socket disappeared"
fi

echo "X11-NAMESPACE-IDENTITY: PASS (uid=${caller_uid} gid=${caller_gid} user=${caller_user})"
