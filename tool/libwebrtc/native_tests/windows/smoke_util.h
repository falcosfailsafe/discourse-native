// Helpers shared by the Windows GPU test programs. Header only.
#ifndef DISCOURSE_NATIVE_TESTS_WINDOWS_SMOKE_UTIL_H_
#define DISCOURSE_NATIVE_TESTS_WINDOWS_SMOKE_UTIL_H_

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace smoke {

inline std::string HrString(const char* what, HRESULT hr) {
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%s failed: 0x%08lX", what,
                static_cast<unsigned long>(hr));
  return buf;
}

[[noreturn]] inline void Die(const std::string& message) {
  std::fprintf(stderr, "FAIL: %s\n", message.c_str());
  std::exit(1);
}

#define CHECK_HR(expr)                           \
  do {                                           \
    HRESULT hr_ = (expr);                        \
    if (FAILED(hr_)) Die(HrString(#expr, hr_));  \
  } while (0)

inline std::string Narrow(const wchar_t* wide) {
  char buf[256] = {};
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, buf, sizeof(buf) - 1, nullptr,
                      nullptr);
  return buf;
}

inline std::string JsonEscape(const std::string& in) {
  std::string out;
  for (char c : in) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n' || c == '\r') {
      out += ' ';
    } else {
      out += c;
    }
  }
  return out;
}

inline std::string RunCommand(const char* command) {
  std::string out;
  FILE* pipe = _popen(command, "r");
  if (!pipe) return "popen failed";
  char buf[256];
  while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
  _pclose(pipe);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
    out.pop_back();
  }
  return out;
}

inline double Percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const size_t i = static_cast<size_t>(p * (v.size() - 1));
  return v[i];
}

inline double Mean(const std::vector<double>& v) {
  if (v.empty()) return 0;
  double sum = 0;
  for (double x : v) sum += x;
  return sum / v.size();
}

struct Sps {
  bool found = false;
  int profile_idc = 0;
  int constraint_flags = 0;
  int level_idc = 0;
  std::string ProfileLevelId() const {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02x%02x%02x", profile_idc,
                  constraint_flags, level_idc);
    return buf;
  }
};

inline Sps FindSps(const std::vector<uint8_t>& data) {
  Sps sps;
  for (size_t i = 0; i + 7 < data.size(); ++i) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 &&
        (data[i + 3] & 0x1f) == 7) {
      sps.found = true;
      sps.profile_idc = data[i + 4];
      sps.constraint_flags = data[i + 5];
      sps.level_idc = data[i + 6];
      return sps;
    }
  }
  return sps;
}

}  // namespace smoke

#endif  // DISCOURSE_NATIVE_TESTS_WINDOWS_SMOKE_UTIL_H_
