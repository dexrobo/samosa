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

# 1. Formatting
log "Running all formatters..."
bazel run //tools/format

# 2. Linting (Ruff via aspect)
log "Running Ruff linter..."
bazel build --config=lint //...

# 3. Type Checking (MyPy)
log "Running MyPy type checker..."
# We run against the package directory.
# MyPy will find the files in the runfiles if they are provided as data.
bazel run //tools/lint:mypy -- dex/infrastructure/shared_memory

# 4. Production Tests
log "Running production tests..."
bazel test --config=prod //...

# 5. Sanitizer Tests
log "Running ASAN tests (static)..."
# Some tests are incompatible with static ASAN due to dlsym;
# they are automatically skipped via target_compatible_with in BUILD files.
bazel test --config=asan //...

log "Running ASAN tests (dynamic)..."
bazel test --config=asan-dynamic //...

log "Running TSAN tests (static)..."
# TSAN requires ASLR to be disabled on some systems/architectures.
bazel test --config=tsan --run_under="setarch $(uname -m) -R" //...

log "Running TSAN tests (dynamic)..."
bazel test --config=tsan-dynamic --run_under="setarch $(uname -m) -R" //...

log "Running UBSAN tests..."
bazel test --config=ubsan //...

log "All checks passed successfully!"
