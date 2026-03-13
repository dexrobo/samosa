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

case "$PART" in
    format)
        log "Running all formatters..."
        bazel run //tools/format
        ;;
    lint)
        log "Running Ruff linter..."
        bazel build --config=lint //...
        log "Running MyPy type checker..."
        bazel run //tools/lint:mypy -- dex/infrastructure/shared_memory
        ;;
    test-prod)
        log "Running production tests..."
        bazel test --config=prod //...
        ;;
    test-asan)
        log "Running ASAN tests (static)..."
        bazel test --config=asan //...
        ;;
    test-asan-dynamic)
        log "Running ASAN tests (dynamic)..."
        bazel test --config=asan-dynamic //...
        ;;
    test-tsan)
        log "Running TSAN tests (static)..."
        bazel test --config=tsan --run_under="setarch $(uname -m) -R" //...
        ;;
    test-tsan-dynamic)
        log "Running TSAN tests (dynamic)..."
        bazel test --config=tsan-dynamic --run_under="setarch $(uname -m) -R" //...
        ;;
    test-ubsan)
        log "Running UBSAN tests..."
        bazel test --config=ubsan //...
        ;;
    all)
        "$0" format
        "$0" lint
        "$0" test-prod
        "$0" test-asan
        "$0" test-asan-dynamic
        "$0" test-tsan
        "$0" test-tsan-dynamic
        "$0" test-ubsan
        ;;
    *)
        error "Unknown check part: $PART"
        exit 1
        ;;
esac

log "Check '$PART' passed successfully!"
