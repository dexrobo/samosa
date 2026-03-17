#!/bin/bash
# Comprehensive check script for Samosa.
#
# Usage:
#   bazel run check -- [all|format|lint|test-prod|test-asan|...] [extra-bazel-args]
#
# Examples:
#   bazel run check -- all                    # Run everything locally
#   bazel run check -- lint --verbose_failures # Run linting with extra flags
#
# Hybrid Logic:
#   This script automatically detects if it's running in a CI environment (GitHub Actions
#   or Docker-in-Docker). If so, it adds '--define=ci=true' to enable optimizations like
#   the REDUCED_TEST_SET for timing-sensitive sanitizer tests.

set -euo pipefail

# Ensure we are in the workspace root
if [[ -n "${BUILD_WORKSPACE_DIRECTORY:-}" ]]; then
    cd "$BUILD_WORKSPACE_DIRECTORY"
else
    cd "$(dirname "$0")/.."
fi

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color

function log() {
    echo -e "${GREEN}[CHECK]${NC} $1"
}

function error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Allow running only a specific part
PART="${1:-all}"
shift || true
EXTRA_ARGS=("$@")

# Internal flag to avoid redundant logs and defines in recursive calls
IS_RECURSIVE="${SAMOSA_CHECK_INTERNAL:-false}"
export SAMOSA_CHECK_INTERNAL=true

# If running in CI, automatically add the CI define to enable optimizations/reduced test sets.
if [[ "${CI:-}" == "true" || "${DOCKER_HOST:-}" == *"docker:2375"* ]]; then
    if [[ "$IS_RECURSIVE" == "false" ]]; then
        log "CI environment detected. Enabling CI optimizations."
    fi
    # Add define if not already present in EXTRA_ARGS
    if [[ ! " ${EXTRA_ARGS[*]:-} " =~ " --define=ci=true " ]]; then
        EXTRA_ARGS=("--define=ci=true" "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}")
    fi
fi

case "$PART" in
    format)
        log "Running all formatters..."
        bazel run //tools/format "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"
        if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
            # Exclude lockfile from formatting check as it may be updated by Bazel resolution
            DIFF_FILES=$(git diff --name-only -- . ':!MODULE.bazel.lock')
            if [[ -n "$DIFF_FILES" ]]; then
                error "Formatting changes detected in the following files:"
                echo "$DIFF_FILES"
                error "Please commit the changes."
                exit 1
            fi
        fi
        ;;
    lint)
        log "Running linters (Ruff, Clang-Tidy)..."
        bazel build --config=lint //... "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"
        log "Running MyPy type checker..."
        # MyPy is run via 'bazel run', so it needs EXTRA_ARGS before the '--' separator.
        bazel run //tools/lint:mypy "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}" -- dex/infrastructure/shared_memory
        ;;
    test-prod)
        log "Running production tests..."
        bazel test --config=prod //... "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"
        ;;
    test-asan)
        log "Running ASAN tests (static)..."
        bazel test --config=asan //... "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"
        ;;
    test-asan-dynamic)
        log "Running ASAN tests (dynamic)..."
        bazel test --config=asan-dynamic //... "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"
        ;;
    test-tsan)
        log "Running TSAN tests (static)..."
        bazel test --config=tsan --run_under="setarch $(uname -m) -R" //... "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"
        ;;
    test-tsan-dynamic)
        log "Running TSAN tests (dynamic)..."
        bazel test --config=tsan-dynamic --run_under="setarch $(uname -m) -R" //... "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"
        ;;
    test-ubsan)
        log "Running UBSAN tests..."
        bazel test --config=ubsan //... "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"
        ;;
    all)
        "$0" format "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"
        "$0" lint "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"
        "$0" test-prod "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"
        "$0" test-asan "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"
        "$0" test-asan-dynamic "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"
        "$0" test-tsan "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"
        "$0" test-tsan-dynamic "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"
        "$0" test-ubsan "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"
        ;;
    *)
        error "Unknown check part: $PART"
        exit 1
        ;;
esac

log "Check '$PART' passed successfully!"
