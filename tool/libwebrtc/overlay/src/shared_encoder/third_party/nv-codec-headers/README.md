`ffnvcodec/nvEncodeAPI.h` from FFmpeg/nv-codec-headers tag `n12.1.14.0`
(commit recorded below), Video Codec SDK 12.1 API. The header carries NVIDIA's
MIT-style notice at its top; it is the only file taken. API 12.1 is chosen on
purpose: it needs NVIDIA driver 531.61 or newer instead of 570 for API 13.
Only the header is vendored; `nvEncodeAPI64.dll` is loaded at run time.
Commit: 1889e62e2d35ff7aa9baca2bceb14f053785e6f1
