#include "test_framework.h"

#include <cstdlib>
#include <cstring>

namespace urftest {

std::vector<Case>& registry() { static std::vector<Case> r; return r; }

int& failures() { static int f = 0; return f; }

void fail(const char* file, int line, const std::string& msg) {
  std::printf("  FAIL %s:%d\n    %s\n", file, line, msg.c_str());
  ++failures();
}

std::string hex(const std::vector<unsigned char>& v) {
  std::string s;
  char buf[4];
  for (unsigned char b : v) { std::snprintf(buf, sizeof buf, "%02x ", b); s += buf; }
  return s;
}

std::string TmpPath(const std::string& name) {
  const char* dir = std::getenv("URF_TEST_TMPDIR");
  return std::string(dir ? dir : "/tmp") + "/" + name;
}

}  // namespace urftest

namespace {

// 峰值常驻内存。用来核实 spec 第 4.2 节「峰值内存由带高决定，与页面尺寸无关」
// 这条主张——A4 整页是 34.8MB，如果哪天有人把编码器改成需要整页缓冲，
// 这个数字会立刻暴涨。macOS 上 /proc 不存在，只在 Linux / Android 上报。
void PrintPeakRss() {
#if defined(__linux__)
  std::FILE* f = std::fopen("/proc/self/status", "r");
  if (!f) return;
  char line[256];
  while (std::fgets(line, sizeof line, f)) {
    if (std::strncmp(line, "VmHWM:", 6) == 0) {
      std::printf("峰值常驻内存%s", line + 6);
      break;
    }
  }
  std::fclose(f);
#endif
}

}  // namespace

int main() {
  for (auto& c : urftest::registry()) {
    std::printf("RUN  %s\n", c.name.c_str());
    c.fn();
  }
  PrintPeakRss();
  int f = urftest::failures();
  if (f) std::printf("\n%d 处失败\n", f);
  else std::printf("\n全部通过\n");
  return f ? 1 : 0;
}
