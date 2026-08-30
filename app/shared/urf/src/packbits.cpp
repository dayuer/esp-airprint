#include "urf/urf.h"

namespace urf {

namespace {
constexpr size_t kMaxRun = 128;
constexpr uint8_t kEndOfLine = 0x80;
}  // namespace

void EncodeRowGray8(const uint8_t* row, size_t width, std::vector<uint8_t>& out) {
  size_t i = 0;
  while (i < width) {
    // 先看当前位置的游程有多长。
    size_t run = 1;
    while (i + run < width && row[i + run] == row[i] && run < kMaxRun) ++run;

    if (run >= 2) {
      out.push_back(static_cast<uint8_t>(run - 1));
      out.push_back(row[i]);
      i += run;
      continue;
    }

    // 游程只有 1，攒一段字面串，直到遇上真正的游程或攒满 128。
    size_t start = i;
    while (i < width && (i - start) < kMaxRun) {
      size_t r = 1;
      while (i + r < width && row[i + r] == row[i]) ++r;
      if (r >= 2) break;   // 后面是游程，交给下一轮
      ++i;
    }

    size_t len = i - start;
    if (len == 1) {
      // 257-1 = 256 会溢出。单个像素退回游程编码。
      out.push_back(0);
      out.push_back(row[start]);
    } else {
      out.push_back(static_cast<uint8_t>(257 - len));
      out.insert(out.end(), row + start, row + i);
    }
  }
  out.push_back(kEndOfLine);
}

}  // namespace urf
