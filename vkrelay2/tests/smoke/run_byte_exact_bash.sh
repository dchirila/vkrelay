#!/usr/bin/env bash
# Regression: `vkrelay2-launch --run` (no --run-output) must stream the child's
# stdout byte-for-byte, adding nothing.
#
# The capture paths normalize line endings, so byte-exactness is checked at the
# OS level: redirect the launcher's stdout to a file and compare against running
# the same program directly. The child (printf) emits NO trailing newline, so an
# added newline or any re-encoding shows up as a cmp mismatch.
#
# Usage: run_byte_exact_bash.sh <launch-bin> <byte-emitter-bin>
set -euo pipefail
export MSYS_NO_PATHCONV=1
export MSYS2_ARG_CONV_EXCL='*'

launch_arg="$1"
emitter_arg="$2"
to_unix() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -u "$1"; else printf '%s' "$1"; fi
}
launch_exec="$(to_unix "$launch_arg")"
emitter_exec="$(to_unix "$emitter_arg")"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
via_launch="$work/via_launch.bin"
direct="$work/direct.bin"

# Native path is passed as an argument to the (native) launcher; the unix form
# is used to exec the emitter directly from this shell.
"$launch_exec" --run -- "$emitter_arg" > "$via_launch"
"$emitter_exec" > "$direct"

if cmp -s "$via_launch" "$direct"; then
    echo "run --run stdout is byte-exact"
    exit 0
fi
echo "MISMATCH: launcher altered child stdout" >&2
echo "--- via launch ---" >&2
cat -A "$via_launch" >&2
echo "--- direct ---" >&2
cat -A "$direct" >&2
exit 1
