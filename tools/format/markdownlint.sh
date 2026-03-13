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

# Find the binary in the rules_js pkg structure
MARKDOWNLINT=$(rlocation "markdownlint-cli2/package/bin/markdownlint-cli2.js")

exec node "$MARKDOWNLINT" "$@"
