load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

# OpenH264 v2.6.0 — Encoder-only, pure C++ (no assembly).
# Assembly (NASM for x86, NEON for ARM) can be added later for performance.
#
# License: BSD-2-Clause
# https://github.com/cisco/openh264

cc_library(
    name = "common",
    srcs = glob(
        ["codec/common/src/*.cpp"],
        exclude = ["codec/common/src/*_test.cpp"],
    ),
    hdrs = glob([
        "codec/common/inc/*.h",
        "codec/api/wels/*.h",
    ]),
    includes = [
        "codec/api/wels",
        "codec/common/inc",
    ],
    # No assembly defines — pure C++ fallback paths.
    # Do NOT define X86_ASM, HAVE_NEON, HAVE_NEON_AARCH64, etc.
    copts = [
        "-Wno-unused-parameter",
        "-Wno-sign-compare",
        "-Wno-missing-field-initializers",
        "-Wno-unknown-warning-option",
    ],
    linkopts = ["-lpthread"],
)

cc_library(
    name = "processing",
    srcs = glob(
        ["codec/processing/src/**/*.cpp"],
        exclude = [
            "codec/processing/src/x86/**",
            "codec/processing/src/arm/**",
            "codec/processing/src/arm64/**",
            "codec/processing/src/mips/**",
            "codec/processing/src/loongarch/**",
        ],
    ),
    hdrs = glob([
        "codec/processing/interface/*.h",
        "codec/processing/src/**/*.h",
    ]),
    includes = [
        "codec/processing/interface",
        "codec/processing/src/common",
        "codec/processing/src/adaptivequantization",
        "codec/processing/src/backgrounddetection",
        "codec/processing/src/complexityanalysis",
        "codec/processing/src/denoise",
        "codec/processing/src/downsample",
        "codec/processing/src/imagerotate",
        "codec/processing/src/scenechangedetection",
        "codec/processing/src/scrolldetection",
        "codec/processing/src/vaacalc",
    ],
    copts = [
        "-Wno-unused-parameter",
        "-Wno-sign-compare",
        "-Wno-missing-field-initializers",
        "-Wno-unknown-warning-option",
    ],
    deps = [":common"],
)

cc_library(
    name = "encoder",
    srcs = glob(
        ["codec/encoder/core/src/*.cpp"],
        exclude = [
            "codec/encoder/core/src/*_test.cpp",
        ],
    ) + glob(
        ["codec/encoder/plus/src/*.cpp"],
        exclude = [
            "codec/encoder/plus/src/DllEntry.cpp",
        ],
    ),
    hdrs = glob([
        "codec/encoder/core/inc/*.h",
        "codec/encoder/plus/inc/*.h",
    ]),
    includes = [
        "codec/encoder/core/inc",
        "codec/encoder/plus/inc",
    ],
    copts = [
        "-Wno-unused-parameter",
        "-Wno-sign-compare",
        "-Wno-missing-field-initializers",
        "-Wno-unknown-warning-option",
    ],
    deps = [
        ":common",
        ":processing",
    ],
)

# Convenience target — use this from application code.
cc_library(
    name = "openh264",
    hdrs = glob(["codec/api/wels/*.h"]),
    includes = ["codec/api/wels"],
    deps = [":encoder"],
)
