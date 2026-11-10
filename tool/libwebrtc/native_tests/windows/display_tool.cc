// Lists GDI displays and can attach a monitor to the desktop and set its mode.
// Used on headless GPU machines with a virtual display driver, where the
// virtual monitor is not part of the desktop until it is attached.
//
//   display_tool list
//   display_tool set \\.\DISPLAYn WIDTH HEIGHT HZ [X Y] [primary]
//   display_tool modes \\.\DISPLAYn          (lists the modes the driver offers)
//   display_tool detach \\.\DISPLAYn         (remove a monitor from the desktop)
//   display_tool rotate \\.\DISPLAYn DEG      (0, 90, 180 or 270)
//   display_tool hdr \\.\DISPLAYn on|off|query (Windows HDR / advanced color)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wingdi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <utility>
#include <vector>
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

// Windows HDR ("advanced color") through the DisplayConfig API.
static int Hdr(const char* gdi_name, const char* action) {
  UINT32 paths = 0, modes = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS) return 1;
  std::vector<DISPLAYCONFIG_PATH_INFO> p(paths);
  std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, p.data(), &modes, m.data(), nullptr) != ERROR_SUCCESS) return 1;
  for (UINT32 i = 0; i < paths; ++i) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
    src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    src.header.size = sizeof(src);
    src.header.adapterId = p[i].sourceInfo.adapterId;
    src.header.id = p[i].sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
    char name[64] = {};
    WideCharToMultiByte(CP_ACP, 0, src.viewGdiDeviceName, -1, name, sizeof(name) - 1, nullptr, nullptr);
    if (std::strcmp(name, gdi_name) != 0) continue;
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info = {};
    info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    info.header.size = sizeof(info);
    info.header.adapterId = p[i].targetInfo.adapterId;
    info.header.id = p[i].targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&info.header) != ERROR_SUCCESS) { std::printf("advanced color info unavailable\n"); return 1; }
    std::printf("%s: supported=%d enabled=%d wideColorEnforced=%d\n", name,
                info.advancedColorSupported ? 1 : 0, info.advancedColorEnabled ? 1 : 0,
                info.wideColorEnforced ? 1 : 0);
    if (std::strcmp(action, "query") == 0) return 0;
    DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE set = {};
    set.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
    set.header.size = sizeof(set);
    set.header.adapterId = p[i].targetInfo.adapterId;
    set.header.id = p[i].targetInfo.id;
    set.enableAdvancedColor = std::strcmp(action, "on") == 0 ? 1 : 0;
    LONG r = DisplayConfigSetDeviceInfo(&set.header);
    std::printf("set advanced color %s -> %ld\n", action, r);
    return r == ERROR_SUCCESS ? 0 : 1;
  }
  std::printf("no active path for %s\n", gdi_name);
  return 1;
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
    if (primary) {
      // Windows only honours a new primary when the old one is moved off
      // (0,0): lay every other attached display out to the right of it.
      long next_x = dm.dmPelsWidth;
      DISPLAY_DEVICEA dd = {sizeof(dd)};
      for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &dd, 0); ++i) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        if (std::strcmp(dd.DeviceName, name) == 0) continue;
        DEVMODEA other = {};
        other.dmSize = sizeof(other);
        if (!EnumDisplaySettingsA(dd.DeviceName, ENUM_CURRENT_SETTINGS, &other)) continue;
        other.dmPosition.x = next_x;
        other.dmPosition.y = 0;
        other.dmFields |= DM_POSITION;
        next_x += other.dmPelsWidth;
        LONG rr = ChangeDisplaySettingsExA(dd.DeviceName, &other, nullptr,
                                           CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
        std::printf("reposition %s to x=%ld -> %ld\n", dd.DeviceName, other.dmPosition.x, rr);
      }
    }
    r = ChangeDisplaySettingsExA(nullptr, nullptr, nullptr, 0, nullptr);
    std::printf("apply -> %ld\n", r);
    return r == DISP_CHANGE_SUCCESSFUL ? 0 : 1;
  }

  if (argc >= 3 && std::strcmp(argv[1], "detach") == 0) {
    DEVMODEA dm = {};
    dm.dmSize = sizeof(dm);
    dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;  // 0x0: detach
    LONG r = ChangeDisplaySettingsExA(argv[2], &dm, nullptr,
                                      CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    std::printf("stage detach %s -> %ld\n", argv[2], r);
    if (r != DISP_CHANGE_SUCCESSFUL) return 1;
    r = ChangeDisplaySettingsExA(nullptr, nullptr, nullptr, 0, nullptr);
    std::printf("apply -> %ld\n", r);
    return r == DISP_CHANGE_SUCCESSFUL ? 0 : 1;
  }
  if (argc >= 4 && std::strcmp(argv[1], "rotate") == 0) {
    const char* name = argv[2];
    const int degrees = std::atoi(argv[3]);
    DEVMODEA dm = {};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsA(name, ENUM_CURRENT_SETTINGS, &dm)) return 1;
    const bool was_portrait = dm.dmDisplayOrientation == DMDO_90 || dm.dmDisplayOrientation == DMDO_270;
    const DWORD want = degrees == 90 ? DMDO_90 : degrees == 180 ? DMDO_180 : degrees == 270 ? DMDO_270 : DMDO_DEFAULT;
    const bool want_portrait = want == DMDO_90 || want == DMDO_270;
    if (was_portrait != want_portrait) std::swap(dm.dmPelsWidth, dm.dmPelsHeight);
    dm.dmDisplayOrientation = want;
    dm.dmFields |= DM_DISPLAYORIENTATION | DM_PELSWIDTH | DM_PELSHEIGHT;
    LONG r = ChangeDisplaySettingsExA(name, &dm, nullptr, CDS_UPDATEREGISTRY, nullptr);
    std::printf("rotate %s to %d -> %ld (now %lux%lu)\n", name, degrees, r, dm.dmPelsWidth, dm.dmPelsHeight);
    return r == DISP_CHANGE_SUCCESSFUL ? 0 : 1;
  }
  if (argc >= 4 && std::strcmp(argv[1], "hdr") == 0) {
    return Hdr(argv[2], argv[3]);
  }
  std::fprintf(stderr, "usage: display_tool list | modes NAME | set NAME W H HZ [X Y] [primary]\n");
  return 2;
}
