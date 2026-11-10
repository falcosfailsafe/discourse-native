// Loads libwebrtc.dll the way the Flutter plugin does (LoadLibrary +
// GetProcAddress, no import library) and reports what the shared-screen API
// says on this machine. Usage: shared_screen_probe.exe <path-to-libwebrtc.dll>
//
// Struct layouts mirror include/rtc_shared_screen.h (API version 1); the probe
// refuses to continue if the DLL reports a different version.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

extern "C" {
struct Capabilities {
  int available;
  char reason[256];
  char adapter_name[128];
  int max_width;
  int max_height;
  int max_frame_rate;
};
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <libwebrtc.dll>\n", argv[0]);
    return 2;
  }
  DWORD session = 0;
  ProcessIdToSessionId(GetCurrentProcessId(), &session);
  std::printf("session id: %lu\n", session);

  HMODULE dll = LoadLibraryA(argv[1]);
  if (!dll) {
    std::printf("FAIL: LoadLibrary(%s) error %lu\n", argv[1], GetLastError());
    return 1;
  }
  std::printf("loaded %s\n", argv[1]);

  using ApiVersionFn = int (*)();
  using CapabilitiesFn = void (*)(Capabilities*);
  auto version = reinterpret_cast<ApiVersionFn>(
      GetProcAddress(dll, "LibWebRTCSharedScreenApiVersion"));
  auto caps = reinterpret_cast<CapabilitiesFn>(
      GetProcAddress(dll, "LibWebRTCSharedScreenGetCapabilities"));
  const bool has_create =
      GetProcAddress(dll, "LibWebRTCSharedScreenCreateSource") != nullptr;
  const bool has_bitrate =
      GetProcAddress(dll, "LibWebRTCSharedScreenSetPeerBitrate") != nullptr;
  std::printf("exports: ApiVersion=%s GetCapabilities=%s CreateSource=%s "
              "SetPeerBitrate=%s\n",
              version ? "yes" : "NO", caps ? "yes" : "NO",
              has_create ? "yes" : "NO", has_bitrate ? "yes" : "NO");
  if (!version || !caps || !has_create || !has_bitrate) return 1;

  const int v = version();
  std::printf("api version: %d\n", v);
  if (v != 1) {
    std::printf("FAIL: unexpected API version\n");
    return 1;
  }
  Capabilities c = {};
  caps(&c);
  std::printf("available: %d\nreason: %s\nadapter: %s\nmax: %dx%d @ %d fps\n",
              c.available, c.reason, c.adapter_name, c.max_width, c.max_height,
              c.max_frame_rate);
  return 0;
}
