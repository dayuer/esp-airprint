#include "test_framework.h"

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

}  // namespace urftest

int main() {
  for (auto& c : urftest::registry()) {
    std::printf("RUN  %s\n", c.name.c_str());
    c.fn();
  }
  int f = urftest::failures();
  if (f) std::printf("\n%d 处失败\n", f);
  else std::printf("\n全部通过\n");
  return f ? 1 : 0;
}
