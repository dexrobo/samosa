load("@rules_cc//cc:defs.bzl", "cc_library")
load("@rules_nasm//nasm:defs.bzl", "nasm_cc_library")

package(default_visibility = ["//visibility:public"])

# OpenH264 v2.6.0 — Encoder-only.
# ARM64: NEON assembly via GAS .S files (compiled by GCC/Clang directly).
# x86_64: SSE/AVX2 assembly via NASM (hermetic, from BCR rules_nasm).
# Other platforms: pure C++ fallback.
#
# License: BSD-2-Clause
# https://github.com/cisco/openh264

OPENH264_COPTS = [
    "-Wno-unused-parameter",
    "-Wno-sign-compare",
    "-Wno-missing-field-initializers",
    "-Wno-unknown-warning-option",
]

PLATFORM_COPTS = select({
    "@platforms//cpu:aarch64": ["-DHAVE_NEON_AARCH64"],
    "@platforms//cpu:x86_64": ["-DX86_ASM", "-DHAVE_AVX2"],
    "//conditions:default": [],
})

# --- x86_64 NASM assembly libraries ---

nasm_cc_library(
    name = "common_x86_asm",
    srcs = glob(
        ["codec/common/x86/*.asm"],
        exclude = ["codec/common/x86/asm_inc.asm"],
    ),
    copts = ["-DUNIX64", "-DHAVE_AVX2"],
    hdrs = ["codec/common/x86/asm_inc.asm"],
    includes = ["codec/common/x86"],
    target_compatible_with = ["@platforms//cpu:x86_64"],
)

nasm_cc_library(
    name = "encoder_x86_asm",
    srcs = glob(["codec/encoder/core/x86/*.asm"]),
    copts = ["-DUNIX64", "-DHAVE_AVX2"],
    hdrs = ["codec/common/x86/asm_inc.asm"],
    includes = ["codec/common/x86"],
    target_compatible_with = ["@platforms//cpu:x86_64"],
)

nasm_cc_library(
    name = "processing_x86_asm",
    srcs = glob(["codec/processing/src/x86/*.asm"]),
    copts = ["-DUNIX64", "-DHAVE_AVX2"],
    hdrs = ["codec/common/x86/asm_inc.asm"],
    includes = ["codec/common/x86"],
    target_compatible_with = ["@platforms//cpu:x86_64"],
)

# --- Main libraries ---

cc_library(
    name = "common",
    srcs = glob(
        ["codec/common/src/*.cpp"],
        exclude = ["codec/common/src/*_test.cpp"],
    ) + select({
        "@platforms//cpu:aarch64": glob(
            ["codec/common/arm64/*.S"],
            exclude = ["codec/common/arm64/arm_arch64_common_macro.S"],
        ),
        "//conditions:default": [],
    }),
    hdrs = glob([
        "codec/common/inc/*.h",
        "codec/api/wels/*.h",
    ]),
    copts = OPENH264_COPTS + PLATFORM_COPTS,
    includes = [
        "codec/api/wels",
        "codec/common/inc",
        "codec/common/arm64",
    ],
    linkopts = ["-lpthread"],
    textual_hdrs = select({
        "@platforms//cpu:aarch64": ["codec/common/arm64/arm_arch64_common_macro.S"] +
                                   glob(["codec/common/arm64/*.h"]),
        "//conditions:default": [],
    }),
    deps = select({
        "@platforms//cpu:x86_64": [":common_x86_asm"],
        "//conditions:default": [],
    }),
)

cc_library(
    name = "processing",
    srcs = glob(
        ["codec/processing/src/**/*.cpp"],
        exclude = [
            "codec/processing/src/x86/**",
            "codec/processing/src/arm/**",
            "codec/processing/src/mips/**",
            "codec/processing/src/loongarch/**",
        ],
    ) + select({
        "@platforms//cpu:aarch64": glob(["codec/processing/src/arm64/*.S"]),
        "//conditions:default": [],
    }),
    hdrs = glob([
        "codec/processing/interface/*.h",
        "codec/processing/src/**/*.h",
    ]),
    copts = OPENH264_COPTS + PLATFORM_COPTS,
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
    deps = [":common"] + select({
        "@platforms//cpu:x86_64": [":processing_x86_asm"],
        "//conditions:default": [],
    }),
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
    ) + select({
        "@platforms//cpu:aarch64": glob(["codec/encoder/core/arm64/*.S"]),
        "//conditions:default": [],
    }),
    hdrs = glob([
        "codec/encoder/core/inc/*.h",
        "codec/encoder/plus/inc/*.h",
    ]),
    copts = OPENH264_COPTS + PLATFORM_COPTS,
    includes = [
        "codec/encoder/core/inc",
        "codec/encoder/plus/inc",
    ],
    deps = [
        ":common",
        ":processing",
    ] + select({
        "@platforms//cpu:x86_64": [":encoder_x86_asm"],
        "//conditions:default": [],
    }),
)

# Convenience target — use this from application code.
cc_library(
    name = "openh264",
    hdrs = glob(["codec/api/wels/*.h"]),
    includes = ["codec/api/wels"],
    deps = [":encoder"],
)
