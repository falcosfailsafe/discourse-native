#!/usr/bin/env bash
# Builds and runs the portable shared-encoder tests (Linux/macOS, no libwebrtc).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -I "$here/overlay/src" \
  "$here/native_tests/shared_encoder_test.cc" -o "$out/shared_encoder_test"
"$out/shared_encoder_test"
# NVENC wrapper: compile-check against the vendored nvEncodeAPI.h (no GPU needed).
"${CXX:-c++}" -std=c++17 -Wall -Wextra -fsyntax-only \
  -I "$here/overlay/src/shared_encoder" \
  -I "$here/overlay/src/shared_encoder/third_party/nv-codec-headers" \
  "$here/overlay/src/shared_encoder/nvenc_h264_encoder.cc"
echo "nvenc_h264_encoder.cc compiles"
