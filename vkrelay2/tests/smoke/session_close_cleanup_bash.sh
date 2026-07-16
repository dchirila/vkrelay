#!/usr/bin/env bash
# Pure launcher-library regression: cleanup must close the authenticated worker session before it
# tears down the private display. A mock launch binary records argv, so this is daemon/display-free.
set -u

here="$(cd "$(dirname "$0")" && pwd)"
lib="${here}/../../linux/launcher/lib_private_session.sh"
if [ ! -f "${lib}" ]; then
    echo "SESSION-CLOSE-CLEANUP: SKIP (lib not found: ${lib})"
    exit 0
fi
# shellcheck source=/dev/null
. "${lib}"

tmp="$(mktemp -d "${TMPDIR:-/tmp}/vkr_close_cleanup.XXXXXX")" || exit 0
trap 'rm -rf "${tmp}"' EXIT
mock="${tmp}/mock-launch"
MOCK_LOG="${tmp}/argv"
export MOCK_LOG
printf '%s\n' '#!/usr/bin/env bash' 'printf "%s\n" "$@" >"${MOCK_LOG}"' >"${mock}"
chmod +x "${mock}"

VKR_SESSION_LAUNCH_BIN="${mock}"
VKR_SESSION_APP_ID="close-cleanup-app"
VKRELAY2_WORKER_ID="wkr-close-7"
VKRELAY2_APP_TOKEN="token-close-7"
export VKR_SESSION_LAUNCH_BIN VKR_SESSION_APP_ID VKRELAY2_WORKER_ID VKRELAY2_APP_TOKEN

vkr_session_cleanup 0

expected="${tmp}/expected"
printf '%s\n' --close-session --app-id close-cleanup-app --worker-id wkr-close-7 \
    --app-token token-close-7 >"${expected}"
if cmp -s "${expected}" "${MOCK_LOG}"; then
    echo "SESSION-CLOSE-CLEANUP: PASS"
    exit 0
fi
echo "SESSION-CLOSE-CLEANUP: FAIL (unexpected close argv)"
diff -u "${expected}" "${MOCK_LOG}" 2>/dev/null || true
exit 1
