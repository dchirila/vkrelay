#!/usr/bin/env bash
# Bash argv/env/cwd preservation smoke test.
#
# Exercises the realistic Linux launch path: a Bash argv array passed after
# "--" to vkrelay2-launch, which builds a structured descriptor, round-trips
# it, and spawns argv-echo. The C++ verifier does all byte comparison so this
# script never parses JSON.
#
# Usage: argv_echo_bash.sh <launch-bin> <argv-echo-bin> <argv-verify-bin>
set -euo pipefail

# On Git Bash (Windows), stop MSYS from rewriting our arguments/paths so the
# shell -> native exe boundary is tested honestly.
export MSYS_NO_PATHCONV=1
export MSYS2_ARG_CONV_EXCL='*'

launch_arg="$1"
echo_bin="$2"
verify_arg="$3"

# Paths used to *execute* native exes must be runnable by this shell; paths
# passed as *arguments* to those exes must be in the OS-native form.
to_native() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else printf '%s' "$1"; fi
}
to_unix() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -u "$1"; else printf '%s' "$1"; fi
}

launch_exec="$(to_unix "$launch_arg")"
verify_exec="$(to_unix "$verify_arg")"

work_unix="$(mktemp -d)"
trap 'rm -rf "$work_unix"' EXIT
cwd_unix="$work_unix/cwd dir"
mkdir -p "$cwd_unix"
expect_unix="$work_unix/expect.nul"
echoed_unix="$work_unix/echoed.json"

# Adversarial argv: $'...' yields real tab/newline bytes.
args=(
    'plain'
    'a b c'
    '"double"'
    "'single'"
    'back\slash'
    'trailing\'
    'q\"mix'
    '$PATH'
    '%PATH%'
    '100% sure'
    'a!b'
    'caret^here'
    'amp&here'
    '(parens)'
    ''
    '--gpu'
    $'tab\there'
    $'nl\nhere'
    $'caf\xc3\xa9 unicode'
)

# Expected app argv as a NUL-separated file (program excluded).
printf '%s\0' "${args[@]}" > "$expect_unix"

env_space='VKR_T_SPACE=a b c'
env_quote='VKR_T_QUOTE=he said "hi"'
env_semi='VKR_T_SEMI=a;b;c'
env_equals='VKR_T_EQUALS=k=v'

"$launch_exec" \
    --cwd "$(to_native "$cwd_unix")" \
    --env "$env_space" \
    --env "$env_quote" \
    --env "$env_semi" \
    --env "$env_equals" \
    --run \
    --run-output "$(to_native "$echoed_unix")" \
    -- "$echo_bin" "${args[@]}"

exec "$verify_exec" \
    --expect-argv-file "$(to_native "$expect_unix")" \
    --expect-env "$env_space" \
    --expect-env "$env_quote" \
    --expect-env "$env_semi" \
    --expect-env "$env_equals" \
    --cwd "$(to_native "$cwd_unix")" \
    --actual-file "$(to_native "$echoed_unix")"
