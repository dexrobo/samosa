load("@rules_cc//cc:defs.bzl", "cc_library")

# cpp-httplib v0.22.0 — Header-only HTTP server/client library.
# Built without SSL (no boringssl dependency).
#
# License: MIT
# https://github.com/yhirose/cpp-httplib

cc_library(
    name = "cpp_httplib",
    hdrs = ["httplib.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
)
