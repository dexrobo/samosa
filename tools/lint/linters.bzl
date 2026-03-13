load("@aspect_rules_lint//lint:clang_tidy.bzl", "lint_clang_tidy_aspect")
load("@aspect_rules_lint//lint:ruff.bzl", "lint_ruff_aspect")

ruff = lint_ruff_aspect(
    binary = "@@//tools/format:ruff",
    configs = ["@@//:.ruff.toml"],
)

clang_tidy = lint_clang_tidy_aspect(
    binary = "@@//tools/lint:clang-tidy",
    configs = ["@@//:.clang-tidy"],
)
