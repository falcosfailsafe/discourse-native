// Lists GDI displays and can attach a monitor to the desktop and set its mode.
// Used on headless GPU machines with a virtual display driver, where the
// virtual monitor is not part of the desktop until it is attached.
//
//   display_tool list
//   display_tool set \\.\DISPLAYn WIDTH HEIGHT HZ [X Y] [primary]
//   display_tool modes \\.\DISPLAYn          (lists the modes the driver offers)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

static void List() {
  DISPLAY_DEVICEA dd = {sizeof(dd)};
  for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &dd, 0); ++i) {
    DEVMODEA dm = {};
    dm.dmSize = sizeof(dm);
    const bool have = EnumDisplaySettingsA(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm);
    std::printf("%s | %s | flags=0x%lx%s%s", dd.DeviceName, dd.DeviceString,
                dd.StateFlags,
                (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) ? " ATTACHED" : "",
                (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? " PRIMARY" : "");
    if (have && dm.dmPelsWidth) {
      std::printf(" | %lux%lu@%luHz at (%ld,%ld)", dm.dmPelsWidth, dm.dmPelsHeight,
                  dm.dmDisplayFrequency, dm.dmPosition.x, dm.dmPosition.y);
    }
    std::printf("\n");
  }
}

static void Modes(const char* name) {
  std::set<std::string> seen;
  DEVMODEA dm = {};
  dm.dmSize = sizeof(dm);
  for (DWORD i = 0; EnumDisplaySettingsA(name, i, &dm); ++i) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%lux%lu@%lu", dm.dmPelsWidth,
                  dm.dmPelsHeight, dm.dmDisplayFrequency);
    if (seen.insert(buf).second) std::printf("%s\n", buf);
  }
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc >= 2 && std::strcmp(argv[1], "list") == 0) {
    List();
    return 0;
  }
  if (argc >= 3 && std::strcmp(argv[1], "modes") == 0) {
    Modes(argv[2]);
    return 0;
  }
  if (argc >= 6 && std::strcmp(argv[1], "set") == 0) {
    const char* name = argv[2];
    DEVMODEA dm = {};
    dm.dmSize = sizeof(dm);
    dm.dmPelsWidth = std::atoi(argv[3]);
    dm.dmPelsHeight = std::atoi(argv[4]);
    dm.dmDisplayFrequency = std::atoi(argv[5]);
    dm.dmBitsPerPel = 32;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
    bool primary = false;
    int x = 0, y = 0;
    bool has_pos = false;
    for (int i = 6; i < argc; ++i) {
      if (std::strcmp(argv[i], "primary") == 0) primary = true;
    }
    if (argc >= 8 && std::strcmp(argv[6], "primary") != 0) {
      x = std::atoi(argv[6]);
      y = std::atoi(argv[7]);
      has_pos = true;
    }
    if (primary) {
      x = 0;
      y = 0;
      has_pos = true;
    }
    if (has_pos) {
      dm.dmPosition.x = x;
      dm.dmPosition.y = y;
      dm.dmFields |= DM_POSITION;
    }
    DWORD flags = CDS_UPDATEREGISTRY | CDS_NORESET;
    if (primary) flags |= CDS_SET_PRIMARY;
    LONG r = ChangeDisplaySettingsExA(name, &dm, nullptr, flags, nullptr);
    std::printf("stage %s -> %ld\n", name, r);
    if (r != DISP_CHANGE_SUCCESSFUL) return 1;
    r = ChangeDisplaySettingsExA(nullptr, nullptr, nullptr, 0, nullptr);
    std::printf("apply -> %ld\n", r);
    return r == DISP_CHANGE_SUCCESSFUL ? 0 : 1;
  }
  std::fprintf(stderr, "usage: display_tool list | modes NAME | set NAME W H HZ [X Y] [primary]\n");
  return 2;
}
