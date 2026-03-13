#!/bin/bash
# Comprehensive check script for Samosa.
# Runs all formatters, linters, and test configurations.

set -euo pipefail

# Ensure we are in the workspace root
cd "$(dirname "$0")/.."

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

# If running in CI, automatically add the CI define to enable optimizations/reduced test sets.
if [[ "${CI:-}" == "true" ]]; then
    log "CI environment detected. Enabling CI optimizations."
    EXTRA_ARGS=("--define=ci=true" "${EXTRA_ARGS[@]}")
fi

case "$PART" in
    format)
        log "Running all formatters..."
        bazel run //tools/format "${EXTRA_ARGS[@]}"
        ;;
    lint)
        log "Running Ruff linter..."
        bazel build --config=lint //... "${EXTRA_ARGS[@]}"
        log "Running MyPy type checker..."
        # MyPy is run via 'bazel run', so it needs EXTRA_ARGS before the '--' separator.
        bazel run //tools/lint:mypy "${EXTRA_ARGS[@]}" -- dex/infrastructure/shared_memory
        ;;
    test-prod)
        log "Running production tests..."
        bazel test --config=prod //... "${EXTRA_ARGS[@]}"
        ;;
    test-asan)
        log "Running ASAN tests (static)..."
        bazel test --config=asan //... "${EXTRA_ARGS[@]}"
        ;;
    test-asan-dynamic)
        log "Running ASAN tests (dynamic)..."
        bazel test --config=asan-dynamic //... "${EXTRA_ARGS[@]}"
        ;;
    test-tsan)
        log "Running TSAN tests (static)..."
        bazel test --config=tsan --run_under="setarch $(uname -m) -R" //... "${EXTRA_ARGS[@]}"
        ;;
    test-tsan-dynamic)
        log "Running TSAN tests (dynamic)..."
        bazel test --config=tsan-dynamic --run_under="setarch $(uname -m) -R" //... "${EXTRA_ARGS[@]}"
        ;;
    test-ubsan)
        log "Running UBSAN tests..."
        bazel test --config=ubsan //... "${EXTRA_ARGS[@]}"
        ;;
    all)
        "$0" format "${EXTRA_ARGS[@]}"
        "$0" lint "${EXTRA_ARGS[@]}"
        "$0" test-prod "${EXTRA_ARGS[@]}"
        "$0" test-asan "${EXTRA_ARGS[@]}"
        "$0" test-asan-dynamic "${EXTRA_ARGS[@]}"
        "$0" test-tsan "${EXTRA_ARGS[@]}"
        "$0" test-tsan-dynamic "${EXTRA_ARGS[@]}"
        "$0" test-ubsan "${EXTRA_ARGS[@]}"
        ;;
    *)
        error "Unknown check part: $PART"
        exit 1
        ;;
esac

log "Check '$PART' passed successfully!"
