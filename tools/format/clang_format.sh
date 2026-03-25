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

CLANG_FORMAT=$(rlocation "llvm_toolchain_llvm/bin/clang-format")

inplace=false
declare -a files=()

for arg in "$@"; do
    case "$arg" in
        -i | --i | --inplace)
            inplace=true
            ;;
        -*) ;;
        *)
            if [[ -f "$arg" ]]; then
                files+=("$arg")
            fi
            ;;
    esac
done

if [[ "$inplace" == true && "${#files[@]}" -gt 0 ]]; then
    python3 - "${files[@]}" <<'PY'
from pathlib import Path
import sys

for raw_path in sys.argv[1:]:
    path = Path(raw_path)
    data = path.read_bytes()
    path.write_bytes(data.rstrip(b"\n") + b"\n\n\n")
PY
fi

exec "$CLANG_FORMAT" "$@"
