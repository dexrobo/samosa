"""Linter definitions for the Samosa project.

These aspects are orchestrated by aspect_rules_lint and can be executed via:
  bazel build --config=lint //...
"""

load("@aspect_rules_lint//lint:clang_tidy.bzl", "lint_clang_tidy_aspect")
load("@aspect_rules_lint//lint:ruff.bzl", "lint_ruff_aspect")

# Ruff linter for Python. 
# Uses the hermetic ruff binary defined in MODULE.bazel.
ruff = lint_ruff_aspect(
    binary = "@@//tools/format:ruff",
    configs = ["@@//:.ruff.toml"],
)

# Clang-Tidy for C++.
# Pointed at the LLVM 21 toolchain and the project's strict .clang-tidy config.
clang_tidy = lint_clang_tidy_aspect(
    binary = "@@//tools/lint:clang-tidy",
    configs = ["@@//:.clang-tidy"],
)
