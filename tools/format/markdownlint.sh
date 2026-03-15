#!/bin/bash
set -euo pipefail

# To get access to rlocation
RUNFILES_SCRIPT=bazel_tools/tools/bash/runfiles/runfiles.bash
source "${RUNFILES_DIR:-/dev/null}/$RUNFILES_SCRIPT" 2>/dev/null ||
    source "$(grep -sm1 "^$RUNFILES_SCRIPT " "${RUNFILES_MANIFEST_FILE:-/dev/null}" | cut -f2- -d' ')" 2>/dev/null ||
    source "$0.runfiles/$RUNFILES_SCRIPT" 2>/dev/null ||
    source "$(grep -sm1 "^$RUNFILES_SCRIPT " "$0.runfiles_manifest" | cut -f2- -d' ')" 2>/dev/null ||
    source "$(grep -sm1 "^$RUNFILES_SCRIPT " "$0.exe.runfiles_manifest" | cut -f2- -d' ')" 2>/dev/null ||
    {
        echo >&2 "ERROR: cannot find $RUNFILES_SCRIPT"
        exit 1
    }

# Try all possible node repo names to find npx
NPX=""
# These are the local repo names as defined in MODULE.bazel use_repo
for repo in nodejs_linux_arm64 nodejs_linux_amd64 nodejs_darwin_arm64 nodejs_darwin_amd64; do
    for path in bin/npx bin/nodejs/bin/npx; do
        PATH_NPX=$(rlocation "$repo/$path")
        if [[ -z "$PATH_NPX" ]]; then
            # Try finding by common canonical names in rules_nodejs
            PATH_NPX=$(rlocation "rules_nodejs++node+$repo/$path")
        fi

        if [[ -n "$PATH_NPX" && -f "$PATH_NPX" ]]; then
            NPX="$PATH_NPX"
            break 2
        fi
    done
done
if [[ -z "$NPX" ]]; then
    echo >&2 "ERROR: cannot find npx in any nodejs toolchain repository"
    exit 1
fi

# Add node to PATH so npx can use it
export PATH="$(dirname "$NPX"):$PATH"

# Filter out --write which aspect_rules_lint passes, since markdownlint-cli2 uses --fix
ARGS=()
for arg in "$@"; do
    if [[ "$arg" != "--write" ]]; then
        ARGS+=("$arg")
    fi
done

# Run markdownlint-cli2 via npx to handle dependencies automatically
exec "$NPX" --yes markdownlint-cli2@0.17.2 "--fix" "${ARGS[@]}"
