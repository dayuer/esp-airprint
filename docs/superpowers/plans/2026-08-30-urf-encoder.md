# URF 编码器实现计划（阶段一）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 写一个跨平台纯 C++ 的 URF 编码器与校验器，能在开发机上编译并跑单测，之后被 iOS/Android 的 TurboModule 共用。

**Architecture:** `app/shared/urf/` 是一个独立的 CMake 项目，不依赖 RN、不依赖任何平台 API。编码器是流式的：调用方逐条带喂灰度像素行，编码器立刻 RLE 并写盘，内存占用与页面尺寸无关。文件头里的页数字段先写占位 0，`Close()` 时 seek 回去回填真实值——这是 `docs/API-cloud-print.md` 第 6 节第 2 条那个坑的落点。

**Tech Stack:** C++17、CMake 4.x、ctest。测试框架自带（一个 40 行的 assert 宏），不引入 Catch2/gtest，避免联网拉依赖。

**上游依据：** `docs/superpowers/specs/2026-08-30-mobile-app-design.md` 第 3、4、6、8 节；`docs/API-cloud-print.md` 第 4.4、6 节；`tools/reference/render.py` 的 `fix_page_count`。

---

## URF 格式基准事实

全部来自 `tools/reference/render.py:51-75`（真实跑通过的扫描器）与 `docs/API-cloud-print.md` 第 7 节的最小样本，不是从网上抄的。

**文件头，12 字节：**

| 偏移 | 长度 | 内容 |
|---|---|---|
| 0 | 8 | `UNIRAST\0` |
| 8 | 4 | 页数，**大端 uint32**。写 0 打印机认为文档为空 |

**每页页头，32 字节：**

| 偏移 | 长度 | 内容 |
|---|---|---|
| 0 | 1 | bits per pixel（灰度 8） |
| 1 | 1 | colorspace |
| 2 | 1 | duplex |
| 3 | 1 | quality |
| 4 | 8 | 保留，填 0 |
| 12 | 4 | 宽（像素），大端 |
| 16 | 4 | 高（像素），大端 |
| 20 | 4 | dpi，大端 |
| 24 | 8 | 保留，填 0 |

> `bpp`、`colorspace`、`dpi` 三个字段的取值在 Task 9 里对着 `cupsfilter` 的真实产物核过再定死。
> 在那之前用本计划给的默认值，且**不要**声称已验证。宽高两个字段是确定的——
> `fix_page_count` 就是靠 `h[12:20]` 逐页扫的。

**每行数据：**

```
[1 字节 行重复计数] [RLE 包...] 0x80
```

RLE 包（像素为单位，灰度下 1 像素 = 1 字节）：

| 首字节 n | 含义 |
|---|---|
| `0 ≤ n ≤ 127` | 接 1 个像素，重复 `n+1` 次（游程 1~128） |
| `n == 128` | 行结束 |
| `129 ≤ n ≤ 255` | 接 `257-n` 个字面像素（字面长度 2~128） |

注意 `n=256` 不存在，所以**字面长度不能是 1**——单个不重复像素必须编码成
游程 1（`0x00`, 像素）。这是最容易写错的一处，Task 4 有专门的测试。

行重复计数：本阶段**恒为 0**（每行独立编码）。用它压缩连续相同行能把空白页从
280KB 降到几百字节，但语义要在真打印机上验过才敢用——见 Task 10。

---

## 文件结构

| 文件 | 职责 |
|---|---|
| `app/shared/urf/CMakeLists.txt` | 库 + 测试的构建定义 |
| `app/shared/urf/include/urf/urf.h` | 公开接口：`PageSpec`、`EncodeRowGray8`、`Writer`、`Validate` |
| `app/shared/urf/src/packbits.cpp` | 单行 RLE 编码。只做这一件事 |
| `app/shared/urf/src/writer.cpp` | 文件头/页头写入、流式喂行、页数回填 |
| `app/shared/urf/src/validate.cpp` | 扫描一个 .urf 文件并报告页数与首页尺寸 |
| `app/shared/urf/tests/test_main.cpp` | 极简断言框架与 main |
| `app/shared/urf/tests/test_packbits.cpp` | 行编码测试 |
| `app/shared/urf/tests/test_writer.cpp` | 写入器测试 |
| `app/shared/urf/tests/test_validate.cpp` | 校验器测试，含文档第 7 节黄金样本 |
| `app/tools/urfdump.py` | 打印任意 .urf 的头部字段，用于核对常量 |

按职责分文件，不按技术分层：RLE 编码和文件写入是两件独立的事，各自可单独测试。

---

## Task 1: CMake 骨架与测试跑道

先让 `ctest` 能跑起来，再写任何真代码。没有跑道就没法 TDD。

**Files:**
- Create: `app/shared/urf/CMakeLists.txt`
- Create: `app/shared/urf/include/urf/urf.h`
- Create: `app/shared/urf/src/packbits.cpp`
- Create: `app/shared/urf/tests/test_framework.h`
- Create: `app/shared/urf/tests/test_main.cpp`

- [ ] **Step 1: 写极简断言框架**

分两个文件：宏和声明放头文件（每个测试 .cpp 都要用到），定义和 `main` 放 .cpp。
写成一个文件的话，其余测试文件看不到 `TEST` 宏，第二个测试文件一加进来就编译失败。

`app/shared/urf/tests/test_framework.h`：

```cpp
#pragma once
// 极简测试框架：不引入 Catch2/gtest，避免为了跑一个编码器单测而联网拉依赖。
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace urftest {

struct Case { std::string name; std::function<void()> fn; };

std::vector<Case>& registry();
int& failures();
void fail(const char* file, int line, const std::string& msg);
std::string hex(const std::vector<unsigned char>& v);

// 用一个构造函数做注册，避免 -Wunused-const-variable。
struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

}  // namespace urftest

#define TEST(name)                                                        \
  static void name();                                                     \
  static const urftest::Registrar name##_reg(#name, name);                \
  static void name()

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) urftest::fail(__FILE__, __LINE__, "CHECK(" #cond ")");   \
  } while (0)

#define CHECK_EQ(a, b)                                                    \
  do {                                                                    \
    auto _a = (a); auto _b = (b);                                         \
    if (!(_a == _b))                                                      \
      urftest::fail(__FILE__, __LINE__,                                   \
                    "CHECK_EQ(" #a ", " #b ") 左=" + std::to_string(_a) + \
                        " 右=" + std::to_string(_b));                     \
  } while (0)

#define CHECK_BYTES(actual, ...)                                          \
  do {                                                                    \
    std::vector<unsigned char> _exp = __VA_ARGS__;                        \
    std::vector<unsigned char> _act = (actual);                           \
    if (_act != _exp)                                                     \
      urftest::fail(__FILE__, __LINE__,                                   \
                    "字节不符\n    实际: " + urftest::hex(_act) +          \
                        "\n    期望: " + urftest::hex(_exp));              \
  } while (0)
```

`app/shared/urf/tests/test_main.cpp`：

```cpp
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
```

- [ ] **Step 2: 写公开头文件（本任务只放 `EncodeRowGray8`）**

`app/shared/urf/include/urf/urf.h`：

```cpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace urf {

// 把一行 8 位灰度像素编码成 URF 的行数据，追加到 out。
// 输出不含前置的「行重复计数」字节，但**含**行尾的 0x80。
// width 为 0 时只追加 0x80。
void EncodeRowGray8(const uint8_t* row, size_t width, std::vector<uint8_t>& out);

}  // namespace urf
```

- [ ] **Step 3: 放一个会失败的桩实现**

`app/shared/urf/src/packbits.cpp`：

```cpp
#include "urf/urf.h"

namespace urf {

void EncodeRowGray8(const uint8_t*, size_t, std::vector<uint8_t>&) {
  // Task 4 实现
}

}  // namespace urf
```

- [ ] **Step 4: 写 CMakeLists**

`app/shared/urf/CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.20)
project(urf CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(urf
  src/packbits.cpp
)
target_include_directories(urf PUBLIC include)
target_compile_options(urf PRIVATE -Wall -Wextra -Werror)

enable_testing()

add_executable(urf_tests
  tests/test_main.cpp
  tests/test_packbits.cpp
)
target_link_libraries(urf_tests PRIVATE urf)
# -include 把测试框架头强制注入每个测试 .cpp，省得每个文件都写一遍 include。
# clang / gcc 都支持；本项目的三个构建环境（开发机、Xcode、NDK）用的都是这两者。
target_compile_options(urf_tests PRIVATE -Wall -Wextra
  -include ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_framework.h)

add_test(NAME urf_tests COMMAND urf_tests)
```

- [ ] **Step 5: 建一个占位测试文件**

`app/shared/urf/tests/test_packbits.cpp`：

```cpp
#include "urf/urf.h"
#include <vector>

// 测试在 Task 4 写。这里先让构建跑通。
```

- [ ] **Step 6: 配置并构建，确认跑道通了**

```bash
cd app/shared/urf && cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

期望：`100% tests passed, 0 tests failed out of 1`，输出里有「全部通过」。

- [ ] **Step 7: 加 .gitignore 并提交**

`app/shared/urf/.gitignore`：

```
build/
```

```bash
git add app/shared/urf
git commit -m "build(urf): C++ 编码器的 CMake 骨架与测试跑道"
```

---

## Task 2: 一条命令跑测试

每次手敲四段 cmake 会让人懒得跑测试。先把它变成一条命令。

**Files:**
- Create: `app/shared/urf/run_tests.sh`

- [ ] **Step 1: 写脚本**

`app/shared/urf/run_tests.sh`：

```bash
#!/usr/bin/env bash
# URF 编码器单测。在开发机上跑，不需要手机、不需要打印机。
set -euo pipefail
cd "$(dirname "$0")"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

- [ ] **Step 2: 加执行权限并运行**

```bash
chmod +x app/shared/urf/run_tests.sh && app/shared/urf/run_tests.sh
```

期望：`100% tests passed`。

- [ ] **Step 3: 提交**

```bash
git add app/shared/urf/run_tests.sh
git commit -m "build(urf): 一条命令跑单测"
```

---

## Task 3: 行编码——游程

先做最简单的一类：整行像素相同。

**Files:**
- Modify: `app/shared/urf/tests/test_packbits.cpp`
- Modify: `app/shared/urf/src/packbits.cpp`

- [ ] **Step 1: 写失败的测试**

把 `app/shared/urf/tests/test_packbits.cpp` 整个替换成：

```cpp
#include "urf/urf.h"
#include <vector>

using Bytes = std::vector<unsigned char>;

static Bytes Encode(const Bytes& row) {
  std::vector<uint8_t> out;
  urf::EncodeRowGray8(row.data(), row.size(), out);
  return Bytes(out.begin(), out.end());
}

// 空行只有结束符。
TEST(空行) {
  CHECK_BYTES(Encode({}), Bytes{0x80});
}

// 单个像素编码成「游程 1」：n=0 表示重复 0+1 次。
TEST(单像素) {
  CHECK_BYTES(Encode({0x41}), Bytes{0x00, 0x41, 0x80});
}

// 3 个相同像素：n = 3-1 = 2。
TEST(短游程) {
  CHECK_BYTES(Encode({0xAA, 0xAA, 0xAA}), Bytes{0x02, 0xAA, 0x80});
}

// 游程上限是 128（n=127）。129 个像素要拆成 128 + 1。
TEST(游程上限) {
  Bytes row(129, 0xFF);
  CHECK_BYTES(Encode(row), Bytes{0x7F, 0xFF, 0x00, 0xFF, 0x80});
}

// A4 600dpi 一整行白：4962 = 128*38 + 98。
TEST(整行白) {
  Bytes row(4962, 0xFF);
  Bytes got = Encode(row);
  Bytes want;
  for (int i = 0; i < 38; ++i) { want.push_back(0x7F); want.push_back(0xFF); }
  want.push_back(97);   // 98-1
  want.push_back(0xFF);
  want.push_back(0x80);
  CHECK_BYTES(got, want);
}
```

- [ ] **Step 2: 跑测试确认失败**

```bash
app/shared/urf/run_tests.sh
```

期望：FAIL，5 处失败，输出里能看到「实际:」是空的或只有部分字节。

- [ ] **Step 3: 实现游程编码**

`app/shared/urf/src/packbits.cpp` 整个替换成：

```cpp
#include "urf/urf.h"

namespace urf {

namespace {
constexpr size_t kMaxRun = 128;
constexpr uint8_t kEndOfLine = 0x80;
}  // namespace

void EncodeRowGray8(const uint8_t* row, size_t width, std::vector<uint8_t>& out) {
  size_t i = 0;
  while (i < width) {
    size_t run = 1;
    while (i + run < width && row[i + run] == row[i] && run < kMaxRun) ++run;
    out.push_back(static_cast<uint8_t>(run - 1));
    out.push_back(row[i]);
    i += run;
  }
  out.push_back(kEndOfLine);
}

}  // namespace urf
```

- [ ] **Step 4: 跑测试确认通过**

```bash
app/shared/urf/run_tests.sh
```

期望：`全部通过`，`100% tests passed`。

- [ ] **Step 5: 提交**

```bash
git add app/shared/urf
git commit -m "feat(urf): 行编码的游程分支"
```

---

## Task 4: 行编码——字面串

上一步对不重复的数据是最坏情况：每个像素花 2 字节，照片行会膨胀一倍。
字面串把它压回接近 1 字节/像素。

**关键陷阱**：字面长度编码成 `257-len`，`len=1` 会得到 256，溢出。
所以长度为 1 的字面串必须退回游程编码。

**Files:**
- Modify: `app/shared/urf/tests/test_packbits.cpp`
- Modify: `app/shared/urf/src/packbits.cpp`

- [ ] **Step 1: 追加失败的测试**

在 `app/shared/urf/tests/test_packbits.cpp` 末尾追加：

```cpp
// 两个不同像素编码成字面串：len=2 → n = 257-2 = 255。
TEST(短字面串) {
  CHECK_BYTES(Encode({0x00, 0xFF}), Bytes{0xFF, 0x00, 0xFF, 0x80});
}

// 游程在前，字面串在后。
TEST(游程加字面串) {
  CHECK_BYTES(Encode({0xAA, 0xAA, 0xAA, 0x01, 0x02}),
              Bytes{0x02, 0xAA, 0xFF, 0x01, 0x02, 0x80});
}

// 单个孤立像素后面跟着游程：字面长度会是 1，必须退回游程编码，
// 否则 257-1=256 溢出成 0x00，解码端会把它当成「重复 1 次」而错位。
TEST(孤立像素退回游程) {
  CHECK_BYTES(Encode({0x01, 0x02, 0x02}),
              Bytes{0x00, 0x01, 0x01, 0x02, 0x80});
}

// 字面串上限 128：n = 257-128 = 129。
TEST(字面串上限) {
  Bytes row;
  for (int i = 0; i < 130; ++i) row.push_back(static_cast<unsigned char>(i));
  Bytes got = Encode(row);
  // 前 128 个是一个字面串，剩下 2 个是另一个。
  CHECK_EQ(static_cast<int>(got[0]), 129);
  CHECK_EQ(static_cast<int>(got[1]), 0);
  CHECK_EQ(static_cast<int>(got[128]), 127);
  CHECK_EQ(static_cast<int>(got[129]), 255);   // 257-2
  CHECK_EQ(static_cast<int>(got[130]), 128);
  CHECK_EQ(static_cast<int>(got[131]), 129);
  CHECK_EQ(static_cast<int>(got[132]), 0x80);
  CHECK_EQ(got.size(), static_cast<size_t>(133));
}

// 编码结果绝不能出现字面长度 1 对应的 0x00 首字节被误用。
// 逐字节检查一段随机但确定的数据能被完整解码回原样。
TEST(往返一致) {
  Bytes row;
  unsigned s = 12345;
  for (int i = 0; i < 1000; ++i) {
    s = s * 1103515245u + 12345u;
    row.push_back(static_cast<unsigned char>((s >> 16) % 4));  // 少量取值，游程和字面串都会出现
  }
  Bytes enc = Encode(row);

  // 按 fix_page_count 的规则解码
  Bytes dec;
  size_t p = 0;
  while (p < enc.size()) {
    unsigned char n = enc[p++];
    if (n == 128) break;
    if (n < 128) {
      for (int k = 0; k <= n; ++k) dec.push_back(enc[p]);
      p += 1;
    } else {
      size_t len = 257 - n;
      for (size_t k = 0; k < len; ++k) dec.push_back(enc[p + k]);
      p += len;
    }
  }
  CHECK_EQ(p, enc.size());
  CHECK_BYTES(dec, row);
}
```

- [ ] **Step 2: 跑测试确认失败**

```bash
app/shared/urf/run_tests.sh
```

期望：`短字面串`、`游程加字面串`、`字面串上限`、`往返一致` 失败（当前实现把每个像素都编成游程）。
`孤立像素退回游程` 会碰巧通过——那正是当前实现的行为。

- [ ] **Step 3: 实现字面串分支**

`app/shared/urf/src/packbits.cpp` 里的 `EncodeRowGray8` 整个替换成：

```cpp
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
```

- [ ] **Step 4: 跑测试确认全部通过**

```bash
app/shared/urf/run_tests.sh
```

期望：`全部通过`，11 个测试用例无失败。

- [ ] **Step 5: 提交**

```bash
git add app/shared/urf
git commit -m "feat(urf): 行编码的字面串分支，长度 1 退回游程避免 257-len 溢出"
```

---

## Task 5: 页头与文件头

**Files:**
- Modify: `app/shared/urf/include/urf/urf.h`
- Create: `app/shared/urf/src/writer.cpp`
- Create: `app/shared/urf/tests/test_writer.cpp`
- Modify: `app/shared/urf/CMakeLists.txt`

- [ ] **Step 1: 扩展头文件**

在 `app/shared/urf/include/urf/urf.h` 的 `EncodeRowGray8` 声明之后、`}  // namespace urf` 之前插入：

```cpp
// colorspace 字段取值。数值待 Task 9 用真实 cupsfilter 产物核对。
constexpr uint8_t kColorspaceGray = 0;

struct PageSpec {
  uint32_t width_px = 0;
  uint32_t height_px = 0;
  uint32_t dpi = 0;
  uint8_t bits_per_pixel = 8;
  uint8_t colorspace = kColorspaceGray;
};

// 12 字节文件头，页数先写占位值。
void WriteFileHeader(std::vector<uint8_t>& out, uint32_t page_count);

// 32 字节页头。
void WritePageHeader(std::vector<uint8_t>& out, const PageSpec& spec);
```

同时在文件顶部的 include 里已有 `<vector>`、`<cstdint>`、`<cstddef>`，无需改动。

- [ ] **Step 2: 写失败的测试**

`app/shared/urf/tests/test_writer.cpp`：

```cpp
#include "urf/urf.h"
#include <vector>

using Bytes = std::vector<unsigned char>;

TEST(文件头是12字节且页数大端) {
  std::vector<uint8_t> out;
  urf::WriteFileHeader(out, 1);
  CHECK_EQ(out.size(), static_cast<size_t>(12));
  CHECK_BYTES(Bytes(out.begin(), out.begin() + 8),
              Bytes{'U', 'N', 'I', 'R', 'A', 'S', 'T', 0x00});
  CHECK_BYTES(Bytes(out.begin() + 8, out.end()),
              Bytes{0x00, 0x00, 0x00, 0x01});
}

TEST(页数258验证大端不是小端) {
  std::vector<uint8_t> out;
  urf::WriteFileHeader(out, 258);
  CHECK_BYTES(Bytes(out.begin() + 8, out.end()),
              Bytes{0x00, 0x00, 0x01, 0x02});
}

TEST(页头是32字节宽高在12和16偏移) {
  urf::PageSpec spec;
  spec.width_px = 4962;
  spec.height_px = 7014;
  spec.dpi = 600;
  spec.bits_per_pixel = 8;
  spec.colorspace = urf::kColorspaceGray;

  std::vector<uint8_t> out;
  urf::WritePageHeader(out, spec);
  CHECK_EQ(out.size(), static_cast<size_t>(32));

  CHECK_EQ(static_cast<int>(out[0]), 8);   // bpp
  CHECK_EQ(static_cast<int>(out[1]), 0);   // colorspace

  // 4962 = 0x00001362
  CHECK_BYTES(Bytes(out.begin() + 12, out.begin() + 16),
              Bytes{0x00, 0x00, 0x13, 0x62});
  // 7014 = 0x00001B66
  CHECK_BYTES(Bytes(out.begin() + 16, out.begin() + 20),
              Bytes{0x00, 0x00, 0x1B, 0x66});
  // 600 = 0x00000258
  CHECK_BYTES(Bytes(out.begin() + 20, out.begin() + 24),
              Bytes{0x00, 0x00, 0x02, 0x58});
}

TEST(页头保留字节必须是零) {
  urf::PageSpec spec;
  spec.width_px = 100;
  spec.height_px = 100;
  spec.dpi = 600;

  std::vector<uint8_t> out;
  urf::WritePageHeader(out, spec);
  for (size_t i = 4; i < 12; ++i) CHECK_EQ(static_cast<int>(out[i]), 0);
  for (size_t i = 24; i < 32; ++i) CHECK_EQ(static_cast<int>(out[i]), 0);
}
```

- [ ] **Step 3: 把新测试文件加进构建**

`app/shared/urf/CMakeLists.txt` 里的 `add_library(urf ...)` 改成：

```cmake
add_library(urf
  src/packbits.cpp
  src/writer.cpp
)
```

`add_executable(urf_tests ...)` 改成：

```cmake
add_executable(urf_tests
  tests/test_main.cpp
  tests/test_packbits.cpp
  tests/test_writer.cpp
)
```

- [ ] **Step 4: 建空的 writer.cpp 让构建能过，跑测试确认失败**

`app/shared/urf/src/writer.cpp`：

```cpp
#include "urf/urf.h"

namespace urf {

void WriteFileHeader(std::vector<uint8_t>&, uint32_t) {}
void WritePageHeader(std::vector<uint8_t>&, const PageSpec&) {}

}  // namespace urf
```

```bash
app/shared/urf/run_tests.sh
```

期望：4 个新测试失败，报 `CHECK_EQ(out.size(), ...) 左=0 右=12`。

- [ ] **Step 5: 实现**

`app/shared/urf/src/writer.cpp` 整个替换成：

```cpp
#include "urf/urf.h"

namespace urf {

namespace {

void PushBE32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v >> 24));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v));
}

}  // namespace

void WriteFileHeader(std::vector<uint8_t>& out, uint32_t page_count) {
  static const uint8_t kMagic[8] = {'U', 'N', 'I', 'R', 'A', 'S', 'T', 0x00};
  out.insert(out.end(), kMagic, kMagic + 8);
  PushBE32(out, page_count);
}

void WritePageHeader(std::vector<uint8_t>& out, const PageSpec& spec) {
  out.push_back(spec.bits_per_pixel);
  out.push_back(spec.colorspace);
  out.push_back(0);   // duplex
  out.push_back(0);   // quality
  for (int i = 0; i < 8; ++i) out.push_back(0);
  PushBE32(out, spec.width_px);
  PushBE32(out, spec.height_px);
  PushBE32(out, spec.dpi);
  for (int i = 0; i < 8; ++i) out.push_back(0);
}

}  // namespace urf
```

- [ ] **Step 6: 跑测试确认通过**

```bash
app/shared/urf/run_tests.sh
```

期望：`全部通过`，15 个测试用例。

- [ ] **Step 7: 提交**

```bash
git add app/shared/urf
git commit -m "feat(urf): 文件头与页头写入，页数与宽高一律大端"
```

---

## Task 6: 流式 Writer 与页数回填

这是 spec 第 4.2 节「逐条带渲染」的落点：调用方一次只交几百行，Writer 立刻编码写盘。
也是 `docs/API-cloud-print.md` 第 6 节第 2 条的落点：页数先写 0，`Close()` 时 seek 回填。

**Files:**
- Modify: `app/shared/urf/include/urf/urf.h`
- Modify: `app/shared/urf/src/writer.cpp`
- Modify: `app/shared/urf/tests/test_writer.cpp`

- [ ] **Step 1: 在头文件里加 Writer**

在 `app/shared/urf/include/urf/urf.h` 顶部的 include 区加：

```cpp
#include <cstdio>
#include <stdexcept>
#include <string>
```

在 `WritePageHeader` 声明之后、`}  // namespace urf` 之前插入：

```cpp
// 流式 URF 写入器。内存占用与页面尺寸无关：每喂进来一批行就立刻编码写盘。
//
// 用法：
//   Writer w("/tmp/a.urf");
//   w.BeginPage(spec);
//   w.WriteRows(band, rows_in_band);   // 可以调用多次，行数累加
//   w.EndPage();                        // 校验累计行数 == height
//   w.Close();                          // 回填真实页数
//
// 任何一步出错都抛 std::runtime_error。析构时若未 Close 会尝试关闭文件但不回填。
class Writer {
 public:
  explicit Writer(const std::string& path);
  ~Writer();

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  void BeginPage(const PageSpec& spec);

  // rows 指向 row_count 行连续的 8 位灰度像素，每行 spec.width_px 字节。
  void WriteRows(const uint8_t* rows, uint32_t row_count);

  void EndPage();
  void Close();

  uint32_t pages() const { return pages_; }
  uint64_t bytes_written() const { return bytes_; }

 private:
  std::FILE* fp_ = nullptr;
  PageSpec spec_{};
  bool in_page_ = false;
  uint32_t rows_done_ = 0;
  uint32_t pages_ = 0;
  uint64_t bytes_ = 0;
  std::vector<uint8_t> buf_;
};
```

- [ ] **Step 2: 写失败的测试**

在 `app/shared/urf/tests/test_writer.cpp` 末尾追加：

```cpp
#include <cstdio>
#include <string>

static Bytes ReadFile(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  CHECK(f != nullptr);
  Bytes out;
  unsigned char chunk[4096];
  size_t n;
  while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0)
    out.insert(out.end(), chunk, chunk + n);
  std::fclose(f);
  return out;
}

static urf::PageSpec SmallSpec(uint32_t w, uint32_t h) {
  urf::PageSpec s;
  s.width_px = w;
  s.height_px = h;
  s.dpi = 600;
  return s;
}

TEST(写一页并回填页数) {
  const std::string path = "/tmp/urf_test_one.urf";
  {
    urf::Writer w(path);
    w.BeginPage(SmallSpec(4, 2));
    std::vector<uint8_t> rows = {0xFF, 0xFF, 0xFF, 0xFF,
                                 0x00, 0x00, 0x00, 0x00};
    w.WriteRows(rows.data(), 2);
    w.EndPage();
    w.Close();
    CHECK_EQ(w.pages(), 1u);
  }
  Bytes d = ReadFile(path);
  // 文件头页数必须是 1，不是占位的 0。
  CHECK_BYTES(Bytes(d.begin() + 8, d.begin() + 12), Bytes{0, 0, 0, 1});
  // 12 字节文件头 + 32 字节页头 = 44，之后是第一行。
  // 第一行：行重复 0x00，游程 4 个 0xFF（n=3），结束符。
  CHECK_BYTES(Bytes(d.begin() + 44, d.begin() + 48),
              Bytes{0x00, 0x03, 0xFF, 0x80});
  // 第二行：行重复 0x00，游程 4 个 0x00（n=3），结束符。
  CHECK_BYTES(Bytes(d.begin() + 48, d.begin() + 52),
              Bytes{0x00, 0x03, 0x00, 0x80});
  CHECK_EQ(d.size(), static_cast<size_t>(52));
}

TEST(写两页页数是2) {
  const std::string path = "/tmp/urf_test_two.urf";
  {
    urf::Writer w(path);
    std::vector<uint8_t> row = {0x10, 0x20};
    for (int p = 0; p < 2; ++p) {
      w.BeginPage(SmallSpec(2, 1));
      w.WriteRows(row.data(), 1);
      w.EndPage();
    }
    w.Close();
    CHECK_EQ(w.pages(), 2u);
  }
  Bytes d = ReadFile(path);
  CHECK_BYTES(Bytes(d.begin() + 8, d.begin() + 12), Bytes{0, 0, 0, 2});
}

TEST(分多批喂行) {
  const std::string path = "/tmp/urf_test_bands.urf";
  {
    urf::Writer w(path);
    w.BeginPage(SmallSpec(2, 4));
    std::vector<uint8_t> band = {0x01, 0x01, 0x02, 0x02};
    w.WriteRows(band.data(), 2);   // 第 1、2 行
    w.WriteRows(band.data(), 2);   // 第 3、4 行
    w.EndPage();
    w.Close();
  }
  Bytes d = ReadFile(path);
  CHECK_BYTES(Bytes(d.begin() + 8, d.begin() + 12), Bytes{0, 0, 0, 1});
  // 4 行，每行 4 字节（重复计数 + 游程 2 字节 + 结束符）
  CHECK_EQ(d.size(), static_cast<size_t>(12 + 32 + 16));
}

TEST(行数不足时EndPage要抛) {
  const std::string path = "/tmp/urf_test_short.urf";
  bool threw = false;
  try {
    urf::Writer w(path);
    w.BeginPage(SmallSpec(2, 4));
    std::vector<uint8_t> band = {0x01, 0x01};
    w.WriteRows(band.data(), 1);
    w.EndPage();   // 只写了 1 行，声明了 4 行
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(行数超出时WriteRows要抛) {
  const std::string path = "/tmp/urf_test_over.urf";
  bool threw = false;
  try {
    urf::Writer w(path);
    w.BeginPage(SmallSpec(2, 1));
    std::vector<uint8_t> band = {0x01, 0x01, 0x02, 0x02};
    w.WriteRows(band.data(), 2);   // 声明 1 行却喂了 2 行
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(零尺寸页要抛) {
  bool threw = false;
  try {
    urf::Writer w("/tmp/urf_test_zero.urf");
    w.BeginPage(SmallSpec(0, 10));
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(一页都没有就Close要抛) {
  bool threw = false;
  try {
    urf::Writer w("/tmp/urf_test_empty.urf");
    w.Close();   // 页数 0 会让打印机认为文档为空
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
}
```

- [ ] **Step 3: 跑测试确认失败**

```bash
app/shared/urf/run_tests.sh
```

期望：编译失败（`Writer` 未实现）或链接失败。先看到失败再实现。

- [ ] **Step 4: 实现 Writer**

在 `app/shared/urf/src/writer.cpp` 的 `WritePageHeader` 之后、`}  // namespace urf` 之前追加：

```cpp
namespace {
constexpr uint32_t kMaxDimension = 30000;   // 与服务端入口校验一致
}  // namespace

Writer::Writer(const std::string& path) {
  fp_ = std::fopen(path.c_str(), "wb");
  if (!fp_) throw std::runtime_error("打不开输出文件: " + path);
  std::vector<uint8_t> head;
  WriteFileHeader(head, 0);   // 页数占位，Close 时回填
  if (std::fwrite(head.data(), 1, head.size(), fp_) != head.size())
    throw std::runtime_error("写文件头失败");
  bytes_ = head.size();
}

Writer::~Writer() {
  if (fp_) std::fclose(fp_);
}

void Writer::BeginPage(const PageSpec& spec) {
  if (!fp_) throw std::runtime_error("Writer 已关闭");
  if (in_page_) throw std::runtime_error("上一页还没 EndPage");
  if (spec.width_px == 0 || spec.width_px >= kMaxDimension ||
      spec.height_px == 0 || spec.height_px >= kMaxDimension)
    throw std::runtime_error("页面尺寸非法");

  std::vector<uint8_t> head;
  WritePageHeader(head, spec);
  if (std::fwrite(head.data(), 1, head.size(), fp_) != head.size())
    throw std::runtime_error("写页头失败");

  bytes_ += head.size();
  spec_ = spec;
  in_page_ = true;
  rows_done_ = 0;
}

void Writer::WriteRows(const uint8_t* rows, uint32_t row_count) {
  if (!in_page_) throw std::runtime_error("还没 BeginPage");
  if (rows_done_ + row_count > spec_.height_px)
    throw std::runtime_error("喂进来的行数超过页高");

  for (uint32_t r = 0; r < row_count; ++r) {
    buf_.clear();
    buf_.push_back(0);   // 行重复计数，本阶段恒为 0
    EncodeRowGray8(rows + static_cast<size_t>(r) * spec_.width_px,
                   spec_.width_px, buf_);
    if (std::fwrite(buf_.data(), 1, buf_.size(), fp_) != buf_.size())
      throw std::runtime_error("写行数据失败");
    bytes_ += buf_.size();
  }
  rows_done_ += row_count;
}

void Writer::EndPage() {
  if (!in_page_) throw std::runtime_error("还没 BeginPage");
  if (rows_done_ != spec_.height_px)
    throw std::runtime_error("本页只写了 " + std::to_string(rows_done_) +
                             " 行，声明的是 " + std::to_string(spec_.height_px));
  in_page_ = false;
  ++pages_;
}

void Writer::Close() {
  if (!fp_) return;
  if (in_page_) throw std::runtime_error("还有一页没 EndPage");
  if (pages_ == 0)
    throw std::runtime_error("一页都没有——页数字段写 0 会让打印机认为文档为空");

  std::vector<uint8_t> count;
  count.push_back(static_cast<uint8_t>(pages_ >> 24));
  count.push_back(static_cast<uint8_t>(pages_ >> 16));
  count.push_back(static_cast<uint8_t>(pages_ >> 8));
  count.push_back(static_cast<uint8_t>(pages_));
  if (std::fseek(fp_, 8, SEEK_SET) != 0)
    throw std::runtime_error("seek 回文件头失败");
  if (std::fwrite(count.data(), 1, 4, fp_) != 4)
    throw std::runtime_error("回填页数失败");

  if (std::fclose(fp_) != 0) { fp_ = nullptr; throw std::runtime_error("关闭文件失败"); }
  fp_ = nullptr;
}
```

- [ ] **Step 5: 跑测试确认通过**

```bash
app/shared/urf/run_tests.sh
```

期望：`全部通过`，22 个测试用例。

- [ ] **Step 6: 提交**

```bash
git add app/shared/urf
git commit -m "feat(urf): 流式 Writer，Close 时 seek 回填真实页数"
```

---

## Task 7: 校验器

上传前自校验用。对应 spec 第 6 节表格里「页头 9~12 字节大端页数非 0」和
「宽高等于 profile 里对应纸张」两行。

**Files:**
- Modify: `app/shared/urf/include/urf/urf.h`
- Create: `app/shared/urf/src/validate.cpp`
- Create: `app/shared/urf/tests/test_validate.cpp`
- Modify: `app/shared/urf/CMakeLists.txt`

- [ ] **Step 1: 在头文件里加 Validate**

在 `app/shared/urf/include/urf/urf.h` 的 `class Writer { ... };` 之后、`}  // namespace urf` 之前插入：

```cpp
struct ValidateResult {
  bool ok = false;
  uint32_t declared_pages = 0;   // 文件头里写的页数
  uint32_t actual_pages = 0;     // 实际扫出来的页数
  uint32_t width_px = 0;         // 首页宽
  uint32_t height_px = 0;        // 首页高
  std::string error;             // ok 为 false 时说明原因
};

// 扫描一个 .urf 文件。扫描逻辑与 tools/reference/render.py 的 fix_page_count 一致，
// 但这里只报告不修改——编码器写出来就该是对的，需要修就说明编码器有 bug。
ValidateResult Validate(const std::string& path);
```

- [ ] **Step 2: 写失败的测试**

`app/shared/urf/tests/test_validate.cpp`：

```cpp
#include "urf/urf.h"
#include <cstdio>
#include <string>
#include <vector>

using Bytes = std::vector<unsigned char>;

static void WriteRaw(const std::string& path, const Bytes& d) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  std::fwrite(d.data(), 1, d.size(), f);
  std::fclose(f);
}

// docs/API-cloud-print.md 第 7 节第五步那份最小 URF：
// 8 字节魔数 + 大端页数 1 + 12 个零 + 宽高 + 12 个零 + 512 字节零。
// 它是服务端入口校验的黄金样本，校验器必须认它。
static Bytes MinimalUrf() {
  Bytes d = {'U', 'N', 'I', 'R', 'A', 'S', 'T', 0x00,
             0x00, 0x00, 0x00, 0x01};
  for (int i = 0; i < 12; ++i) d.push_back(0);
  // 4962 = 0x00001362, 7014 = 0x00001B66
  Bytes wh = {0x00, 0x00, 0x13, 0x62, 0x00, 0x00, 0x1B, 0x66};
  d.insert(d.end(), wh.begin(), wh.end());
  for (int i = 0; i < 12; ++i) d.push_back(0);
  for (int i = 0; i < 512; ++i) d.push_back(0);
  return d;
}

TEST(黄金样本能被识别出尺寸) {
  const std::string path = "/tmp/urf_golden.urf";
  WriteRaw(path, MinimalUrf());
  urf::ValidateResult r = urf::Validate(path);
  CHECK_EQ(r.declared_pages, 1u);
  CHECK_EQ(r.width_px, 4962u);
  CHECK_EQ(r.height_px, 7014u);
}

TEST(魔数不对要报错) {
  const std::string path = "/tmp/urf_badmagic.urf";
  Bytes d = MinimalUrf();
  d[0] = '%';   // 冒充 PDF
  WriteRaw(path, d);
  urf::ValidateResult r = urf::Validate(path);
  CHECK(!r.ok);
  CHECK(r.error.find("魔数") != std::string::npos);
}

TEST(页数字段为零要报错) {
  const std::string path = "/tmp/urf_zeropages.urf";
  Bytes d = MinimalUrf();
  d[11] = 0;   // 页数改成 0
  WriteRaw(path, d);
  urf::ValidateResult r = urf::Validate(path);
  CHECK(!r.ok);
  CHECK(r.error.find("页数") != std::string::npos);
}

TEST(尺寸越界要报错) {
  const std::string path = "/tmp/urf_bigdim.urf";
  Bytes d = MinimalUrf();
  d[12 + 12] = 0xFF;   // 宽度高位拉满，远超 30000
  d[12 + 13] = 0xFF;
  WriteRaw(path, d);
  urf::ValidateResult r = urf::Validate(path);
  CHECK(!r.ok);
}

TEST(Writer的产物必须自洽) {
  const std::string path = "/tmp/urf_roundtrip.urf";
  {
    urf::PageSpec s;
    s.width_px = 200;
    s.height_px = 50;
    s.dpi = 600;
    std::vector<uint8_t> band(200 * 50);
    for (size_t i = 0; i < band.size(); ++i)
      band[i] = static_cast<uint8_t>(i % 7 == 0 ? 0x00 : 0xFF);

    urf::Writer w(path);
    for (int p = 0; p < 3; ++p) {
      w.BeginPage(s);
      w.WriteRows(band.data(), 50);
      w.EndPage();
    }
    w.Close();
  }
  urf::ValidateResult r = urf::Validate(path);
  CHECK(r.ok);
  CHECK_EQ(r.declared_pages, 3u);
  CHECK_EQ(r.actual_pages, 3u);
  CHECK_EQ(r.width_px, 200u);
  CHECK_EQ(r.height_px, 50u);
}
```

- [ ] **Step 3: 加进构建，建空实现，跑测试确认失败**

`app/shared/urf/CMakeLists.txt`：`add_library` 加 `src/validate.cpp`，`add_executable` 加 `tests/test_validate.cpp`。

`app/shared/urf/src/validate.cpp`：

```cpp
#include "urf/urf.h"

namespace urf {

ValidateResult Validate(const std::string&) { return {}; }

}  // namespace urf
```

```bash
app/shared/urf/run_tests.sh
```

期望：5 个新测试失败。

- [ ] **Step 4: 实现校验器**

`app/shared/urf/src/validate.cpp` 整个替换成：

```cpp
#include "urf/urf.h"

namespace urf {

namespace {

constexpr uint32_t kMaxDimension = 30000;

uint32_t ReadBE32(const std::vector<uint8_t>& d, size_t off) {
  return (static_cast<uint32_t>(d[off]) << 24) |
         (static_cast<uint32_t>(d[off + 1]) << 16) |
         (static_cast<uint32_t>(d[off + 2]) << 8) |
         static_cast<uint32_t>(d[off + 3]);
}

}  // namespace

ValidateResult Validate(const std::string& path) {
  ValidateResult r;

  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) { r.error = "打不开文件: " + path; return r; }
  std::vector<uint8_t> d;
  uint8_t chunk[65536];
  size_t n;
  while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0)
    d.insert(d.end(), chunk, chunk + n);
  std::fclose(f);

  static const uint8_t kMagic[8] = {'U', 'N', 'I', 'R', 'A', 'S', 'T', 0x00};
  if (d.size() < 12) { r.error = "文件不足 12 字节，连文件头都不够"; return r; }
  for (int i = 0; i < 8; ++i) {
    if (d[i] != kMagic[i]) { r.error = "魔数不匹配，期望 UNIRAST\\0"; return r; }
  }

  r.declared_pages = ReadBE32(d, 8);
  if (r.declared_pages == 0) {
    r.error = "页数字段为 0，打印机会认为文档为空";
    return r;
  }

  // 逐页扫描。规则与 tools/reference/render.py 的 fix_page_count 一致。
  size_t pos = 12;
  while (pos + 32 <= d.size()) {
    uint32_t w = ReadBE32(d, pos + 12);
    uint32_t h = ReadBE32(d, pos + 16);
    if (w == 0 || w >= kMaxDimension || h == 0 || h >= kMaxDimension) break;
    if (r.actual_pages == 0) { r.width_px = w; r.height_px = h; }
    ++r.actual_pages;
    pos += 32;

    uint32_t rows = 0;
    while (rows < h && pos < d.size()) {
      ++pos;      // 行重复计数
      ++rows;
      while (pos < d.size()) {
        uint8_t c = d[pos++];
        if (c == 128) break;
        pos += (c < 128) ? 1 : static_cast<size_t>(257 - c);
      }
    }
    if (rows != h) { r.error = "第 " + std::to_string(r.actual_pages) +
                               " 页的行数据不完整"; return r; }
  }

  if (r.actual_pages == 0) { r.error = "扫不出任何一页，首页尺寸非法"; return r; }
  if (r.actual_pages != r.declared_pages) {
    r.error = "页数字段是 " + std::to_string(r.declared_pages) +
              "，实际扫出 " + std::to_string(r.actual_pages) + " 页";
    return r;
  }

  r.ok = true;
  return r;
}

}  // namespace urf
```

- [ ] **Step 5: 跑测试**

```bash
app/shared/urf/run_tests.sh
```

期望：`全部通过`，27 个测试用例。

> 注：`黄金样本能被识别出尺寸` 只断言了 `declared_pages` 和宽高，没断言 `ok`。
> 那份最小样本的行数据是 512 个零字节，凑不满 7014 行，所以 `ok` 会是 false ——
> 这是对的，服务端只校验头部，而我们的校验器要求整份自洽。

- [ ] **Step 6: 提交**

```bash
git add app/shared/urf
git commit -m "feat(urf): 校验器，扫描规则与 fix_page_count 对齐"
```

---

## Task 8: 尺寸断言 API

spec 第 6 节：「宽高等于 profile 里对应纸张——光栅前断言，不等不发请求」。
把这条做成一个显式函数，而不是散在调用方。

**Files:**
- Modify: `app/shared/urf/include/urf/urf.h`
- Modify: `app/shared/urf/src/validate.cpp`
- Modify: `app/shared/urf/tests/test_validate.cpp`

- [ ] **Step 1: 加声明**

在 `app/shared/urf/include/urf/urf.h` 的 `ValidateResult Validate(...)` 声明之后插入：

```cpp
// 在上传之前调用。除了整份自洽，还要求首页尺寸等于 render-profile 给的值。
//
// 这道断言的意义：URF 是按某台打印机的 dpi 和像素尺寸光栅的，尺寸错了服务端会
// 返回 400，漏过去就是错位或半页。而服务端不解析文档、设备不认识格式，
// 没有任何环节会发现——所以本地必须自己拦。
ValidateResult ValidateForUpload(const std::string& path,
                                 uint32_t expect_width_px,
                                 uint32_t expect_height_px);
```

- [ ] **Step 2: 写失败的测试**

在 `app/shared/urf/tests/test_validate.cpp` 末尾追加：

```cpp
static std::string MakeThreePageFile(uint32_t w, uint32_t h) {
  const std::string path = "/tmp/urf_upload_check.urf";
  urf::PageSpec s;
  s.width_px = w;
  s.height_px = h;
  s.dpi = 600;
  std::vector<uint8_t> band(static_cast<size_t>(w) * h, 0xFF);
  urf::Writer wr(path);
  wr.BeginPage(s);
  wr.WriteRows(band.data(), h);
  wr.EndPage();
  wr.Close();
  return path;
}

TEST(尺寸一致时通过) {
  std::string p = MakeThreePageFile(120, 30);
  urf::ValidateResult r = urf::ValidateForUpload(p, 120, 30);
  CHECK(r.ok);
}

TEST(宽度不一致时拒绝且报出期望值) {
  std::string p = MakeThreePageFile(120, 30);
  urf::ValidateResult r = urf::ValidateForUpload(p, 4962, 30);
  CHECK(!r.ok);
  CHECK(r.error.find("4962") != std::string::npos);
  CHECK(r.error.find("120") != std::string::npos);
}

TEST(高度不一致时拒绝) {
  std::string p = MakeThreePageFile(120, 30);
  urf::ValidateResult r = urf::ValidateForUpload(p, 120, 7014);
  CHECK(!r.ok);
}
```

- [ ] **Step 3: 跑测试确认失败**

```bash
app/shared/urf/run_tests.sh
```

期望：链接失败，`ValidateForUpload` 未定义。

- [ ] **Step 4: 实现**

在 `app/shared/urf/src/validate.cpp` 的 `Validate` 函数之后、`}  // namespace urf` 之前追加：

```cpp
ValidateResult ValidateForUpload(const std::string& path,
                                 uint32_t expect_width_px,
                                 uint32_t expect_height_px) {
  ValidateResult r = Validate(path);
  if (!r.ok) return r;

  if (r.width_px != expect_width_px || r.height_px != expect_height_px) {
    r.ok = false;
    r.error = "尺寸与 render-profile 不符：期望 " +
              std::to_string(expect_width_px) + "x" +
              std::to_string(expect_height_px) + "，实际 " +
              std::to_string(r.width_px) + "x" + std::to_string(r.height_px);
  }
  return r;
}
```

- [ ] **Step 5: 跑测试**

```bash
app/shared/urf/run_tests.sh
```

期望：`全部通过`，30 个测试用例。

- [ ] **Step 6: 提交**

```bash
git add app/shared/urf
git commit -m "feat(urf): 上传前的尺寸断言，错了直接给出期望值与实际值"
```

---

## Task 9: 核对页头常量

`bpp`、`colorspace`、`dpi` 三个字段目前是按格式文档填的，没在真实产物上核过。
这一步做一个转储工具并留下核对记录。**不核对就不要声称编码器已验证。**

**Files:**
- Create: `app/tools/urfdump.py`
- Create: `app/shared/urf/HEADER-FIELDS.md`

- [ ] **Step 1: 写转储工具**

`app/tools/urfdump.py`：

```python
#!/usr/bin/env python3
"""打印 .urf 文件的头部字段。用来核对编码器写的常量和真实产物是否一致。

    ./urfdump.py a.urf

字节布局的依据是 tools/reference/render.py 的 fix_page_count。
"""
import struct
import sys


def dump(path):
    d = open(path, 'rb').read()
    if d[:8] != b'UNIRAST\0':
        print('不是 URF：前 8 字节是 %r' % d[:8])
        return 1
    print('文件      %s  %d 字节' % (path, len(d)))
    print('页数字段  %d' % struct.unpack('>I', d[8:12])[0])

    pos, page = 12, 0
    while pos + 32 <= len(d):
        h = d[pos:pos + 32]
        w, ht = struct.unpack('>II', h[12:20])
        if not (0 < w < 30000 and 0 < ht < 30000):
            break
        page += 1
        dpi = struct.unpack('>I', h[20:24])[0]
        print('第 %d 页  bpp=%d colorspace=%d duplex=%d quality=%d '
              '尺寸=%dx%d dpi=%d' % (page, h[0], h[1], h[2], h[3], w, ht, dpi))
        print('        保留字节 [4:12]=%s [24:32]=%s' % (h[4:12].hex(), h[24:32].hex()))

        pos += 32
        rows = 0
        while rows < ht and pos < len(d):
            pos += 1
            rows += 1
            while pos < len(d):
                n = d[pos]
                pos += 1
                if n == 128:
                    break
                pos += 1 if n < 128 else (257 - n)
        print('        行数据扫出 %d 行' % rows)
    print('实际页数  %d' % page)
    return 0


if __name__ == '__main__':
    sys.exit(dump(sys.argv[1]))
```

- [ ] **Step 2: 对着自己的产物跑一遍**

```bash
chmod +x app/tools/urfdump.py && app/shared/urf/run_tests.sh >/dev/null && python3 app/tools/urfdump.py /tmp/urf_roundtrip.urf
```

期望输出里有 `页数字段  3`、`bpp=8 colorspace=0`、`尺寸=200x50 dpi=600`、`实际页数  3`。

- [ ] **Step 3: 记录待核对项**

`app/shared/urf/HEADER-FIELDS.md`：

```markdown
# URF 页头字段的核对状态

编码器写出去的每个页头字段，这里记它的依据和核对状态。
**没核对过的字段不要在别处声称已验证。**

| 字段 | 偏移 | 当前取值 | 依据 | 核对状态 |
|---|---|---|---|---|
| bits per pixel | 0 | 8 | 打印机能力串 `W8` = 8 位灰度 | 未核对 |
| colorspace | 1 | 0 | 格式文档；`W8` 是灰度 | **未核对，风险最高** |
| duplex | 2 | 0 | 单面 | 未核对 |
| quality | 3 | 0 | 默认 | 未核对 |
| 保留 | 4–11 | 全 0 | `docs/API-cloud-print.md` 第 7 节最小样本此处全 0 | 已核对 |
| 宽 | 12–15 | 大端 uint32 | `fix_page_count` 用 `h[12:20]` 逐页扫 | **已核对** |
| 高 | 16–19 | 大端 uint32 | 同上 | **已核对** |
| dpi | 20–23 | 大端 uint32 | 格式文档 | 未核对 |
| 保留 | 24–31 | 全 0 | 同上第 7 节样本 | 已核对 |

## 怎么核对

在装了 CUPS 的机器上，用 `tools/reference/render.py` 的路径产出一份真实 URF，
再用 `app/tools/urfdump.py` 转储，把上表的「当前取值」逐个对上：

    cupsfilter -P <PPD> -m image/urf sample.pdf > /tmp/real.urf
    python3 app/tools/urfdump.py /tmp/real.urf

对不上就改 `app/shared/urf/include/urf/urf.h` 里的常量，并更新本表。

`colorspace` 是风险最高的一个：填错不会报错，打印机可能出一沓乱码纸或整页黑。
第一次真机打印之前必须核对它。
```

- [ ] **Step 4: 提交**

```bash
git add app/tools/urfdump.py app/shared/urf/HEADER-FIELDS.md
git commit -m "tools(urf): 页头转储工具与常量核对清单"
```

---

## Task 10: 行重复压缩（默认关闭）

空白行占了文字页的绝大部分。行重复计数能把 A4 空白页从约 280KB 压到几百字节。
但这个字节的语义（是「重复次数」还是「总出现次数」）没在真机上验过，
**默认关闭**，等 Task 9 的真实产物核对完再打开。

**Files:**
- Modify: `app/shared/urf/include/urf/urf.h`
- Modify: `app/shared/urf/src/writer.cpp`
- Modify: `app/shared/urf/tests/test_writer.cpp`
- Modify: `app/shared/urf/HEADER-FIELDS.md`

- [ ] **Step 1: 加开关到头文件**

把 `app/shared/urf/include/urf/urf.h` 里 `class Writer` 的构造函数声明改成：

```cpp
  // line_repeat 打开后，连续相同的行会合并成一个行重复计数。
  // 默认关闭：这个字节的语义还没在真打印机上验过，见 HEADER-FIELDS.md。
  explicit Writer(const std::string& path, bool line_repeat = false);
```

并在私有成员区 `std::vector<uint8_t> buf_;` 之后追加：

```cpp
  bool line_repeat_ = false;
  std::vector<uint8_t> pending_;      // 上一行的编码结果（不含重复计数）
  uint32_t pending_count_ = 0;        // 上一行重复了几次，0 表示没有待写的行
  void FlushPending();
```

- [ ] **Step 2: 写失败的测试**

在 `app/shared/urf/tests/test_writer.cpp` 末尾追加：

```cpp
TEST(默认关闭时每行都有独立的重复计数零) {
  const std::string path = "/tmp/urf_norepeat.urf";
  {
    urf::Writer w(path);   // 默认关闭
    w.BeginPage(SmallSpec(2, 3));
    std::vector<uint8_t> band = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11};
    w.WriteRows(band.data(), 3);
    w.EndPage();
    w.Close();
  }
  Bytes d = ReadFile(path);
  // 3 行相同，每行 4 字节：00 01 11 80
  CHECK_EQ(d.size(), static_cast<size_t>(12 + 32 + 12));
  CHECK_BYTES(Bytes(d.begin() + 44, d.begin() + 48), Bytes{0x00, 0x01, 0x11, 0x80});
  CHECK_BYTES(Bytes(d.begin() + 48, d.begin() + 52), Bytes{0x00, 0x01, 0x11, 0x80});
}

TEST(打开后三行相同合并成一条) {
  const std::string path = "/tmp/urf_repeat.urf";
  {
    urf::Writer w(path, /*line_repeat=*/true);
    w.BeginPage(SmallSpec(2, 3));
    std::vector<uint8_t> band = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11};
    w.WriteRows(band.data(), 3);
    w.EndPage();
    w.Close();
  }
  Bytes d = ReadFile(path);
  // 重复计数 2 表示「再重复 2 次」，共 3 行。
  CHECK_EQ(d.size(), static_cast<size_t>(12 + 32 + 4));
  CHECK_BYTES(Bytes(d.begin() + 44, d.begin() + 48), Bytes{0x02, 0x01, 0x11, 0x80});
}

TEST(打开后不同的行不合并) {
  const std::string path = "/tmp/urf_repeat_mixed.urf";
  {
    urf::Writer w(path, /*line_repeat=*/true);
    w.BeginPage(SmallSpec(2, 3));
    std::vector<uint8_t> band = {0x11, 0x11,   // 行 1
                                 0x11, 0x11,   // 行 2 与行 1 相同
                                 0x22, 0x22};  // 行 3 不同
    w.WriteRows(band.data(), 3);
    w.EndPage();
    w.Close();
  }
  Bytes d = ReadFile(path);
  CHECK_EQ(d.size(), static_cast<size_t>(12 + 32 + 8));
  CHECK_BYTES(Bytes(d.begin() + 44, d.begin() + 48), Bytes{0x01, 0x01, 0x11, 0x80});
  CHECK_BYTES(Bytes(d.begin() + 48, d.begin() + 52), Bytes{0x00, 0x01, 0x22, 0x80});
}

TEST(重复计数上限256行后另起一条) {
  const std::string path = "/tmp/urf_repeat_cap.urf";
  {
    urf::Writer w(path, /*line_repeat=*/true);
    w.BeginPage(SmallSpec(2, 300));
    std::vector<uint8_t> band(600, 0x33);
    w.WriteRows(band.data(), 300);
    w.EndPage();
    w.Close();
  }
  Bytes d = ReadFile(path);
  // 300 行 = 256 + 44，两条记录，每条 4 字节。
  CHECK_EQ(d.size(), static_cast<size_t>(12 + 32 + 8));
  CHECK_EQ(static_cast<int>(d[44]), 255);   // 再重复 255 次 = 256 行
  CHECK_EQ(static_cast<int>(d[48]), 43);    // 再重复 43 次 = 44 行
}
```

- [ ] **Step 3: 跑测试确认失败**

```bash
app/shared/urf/run_tests.sh
```

期望：编译失败（`Writer` 没有两参数构造函数）。

- [ ] **Step 4: 实现**

`app/shared/urf/src/writer.cpp` 里，把 `Writer::Writer`、`Writer::WriteRows`、`Writer::EndPage`
三个函数替换成下面这组，并新增 `Writer::FlushPending`：

```cpp
Writer::Writer(const std::string& path, bool line_repeat)
    : line_repeat_(line_repeat) {
  fp_ = std::fopen(path.c_str(), "wb");
  if (!fp_) throw std::runtime_error("打不开输出文件: " + path);
  std::vector<uint8_t> head;
  WriteFileHeader(head, 0);
  if (std::fwrite(head.data(), 1, head.size(), fp_) != head.size())
    throw std::runtime_error("写文件头失败");
  bytes_ = head.size();
}

void Writer::FlushPending() {
  if (pending_count_ == 0) return;
  uint8_t repeat = static_cast<uint8_t>(pending_count_ - 1);
  if (std::fwrite(&repeat, 1, 1, fp_) != 1)
    throw std::runtime_error("写行重复计数失败");
  if (std::fwrite(pending_.data(), 1, pending_.size(), fp_) != pending_.size())
    throw std::runtime_error("写行数据失败");
  bytes_ += 1 + pending_.size();
  pending_count_ = 0;
}

void Writer::WriteRows(const uint8_t* rows, uint32_t row_count) {
  if (!in_page_) throw std::runtime_error("还没 BeginPage");
  if (rows_done_ + row_count > spec_.height_px)
    throw std::runtime_error("喂进来的行数超过页高");

  for (uint32_t r = 0; r < row_count; ++r) {
    buf_.clear();
    EncodeRowGray8(rows + static_cast<size_t>(r) * spec_.width_px,
                   spec_.width_px, buf_);

    // 重复计数字段只有 1 字节，最多表示 256 行。
    if (line_repeat_ && pending_count_ > 0 && pending_count_ < 256 &&
        buf_ == pending_) {
      ++pending_count_;
      continue;
    }
    FlushPending();
    pending_ = buf_;
    pending_count_ = 1;
    if (!line_repeat_) FlushPending();
  }
  rows_done_ += row_count;
}

void Writer::EndPage() {
  if (!in_page_) throw std::runtime_error("还没 BeginPage");
  FlushPending();
  if (rows_done_ != spec_.height_px)
    throw std::runtime_error("本页只写了 " + std::to_string(rows_done_) +
                             " 行，声明的是 " + std::to_string(spec_.height_px));
  in_page_ = false;
  ++pages_;
}
```

- [ ] **Step 5: 跑测试**

```bash
app/shared/urf/run_tests.sh
```

期望：`全部通过`，34 个测试用例。

> 注意 `Validate` 会把每个行重复计数只算作 1 行——这与 `fix_page_count` 的行为一致。
> 所以 `line_repeat=true` 写出的文件用 `Validate` 扫会报「行数据不完整」。
> 这是已知的、故意保留的行为：校验器对齐的是服务端和参考实现的口径。
> Task 9 核对完之后如果决定默认打开行重复，必须同时改校验器——写进下面的表格里。

- [ ] **Step 6: 更新核对清单**

在 `app/shared/urf/HEADER-FIELDS.md` 末尾追加：

```markdown
## 行重复计数

| 项 | 状态 |
|---|---|
| 语义假设 | 值 N 表示该行再重复 N 次，共出现 N+1 行 |
| 默认 | **关闭**（`Writer(path)` 的第二参数默认 false） |
| 核对方式 | 用 `cupsfilter` 产出一份含大片空白的真实 URF，`urfdump.py` 看行数扫描是否与页高一致 |
| 打开的前提 | 上面核对通过，**并且**同步修改 `Validate` 让它按 N+1 计行 |

不核对就打开的后果：打印机按错误的行数解码，出半页或整页错位，而本地校验器
会说一切正常。
```

- [ ] **Step 7: 提交**

```bash
git add app/shared/urf
git commit -m "feat(urf): 行重复压缩，默认关闭直到真机核对"
```

---

## Task 11: 内存占用回归测试

spec 第 4.2 节的核心主张是「峰值内存由带高决定，与页面尺寸无关」。
这个主张必须有测试守着，否则将来有人图省事把整页读进 `std::vector` 就没人发现了。

**Files:**
- Modify: `app/shared/urf/tests/test_writer.cpp`

- [ ] **Step 1: 写测试**

在 `app/shared/urf/tests/test_writer.cpp` 末尾追加：

```cpp
// A4 600dpi 灰度整页是 4962x7014 = 34.8MB。这个测试按 400 行一带喂进去，
// 全程不构造整页缓冲。它守的是「峰值内存与页面尺寸无关」这条设计主张：
// 谁把 Writer 改成需要整页缓冲，这里就会因为内存暴涨而变慢或失败。
TEST(A4整页按条带写入) {
  const std::string path = "/tmp/urf_a4.urf";
  const uint32_t kW = 4962, kH = 7014, kBand = 400;

  std::vector<uint8_t> band(static_cast<size_t>(kW) * kBand);
  {
    urf::PageSpec s;
    s.width_px = kW;
    s.height_px = kH;
    s.dpi = 600;

    urf::Writer w(path);
    w.BeginPage(s);
    uint32_t done = 0;
    while (done < kH) {
      uint32_t rows = (kH - done < kBand) ? (kH - done) : kBand;
      // 每带内容不同，避免编码器走上什么捷径。
      for (size_t i = 0; i < static_cast<size_t>(kW) * rows; ++i)
        band[i] = static_cast<uint8_t>((i + done) % 251);
      w.WriteRows(band.data(), rows);
      done += rows;
    }
    w.EndPage();
    w.Close();
  }

  urf::ValidateResult r = urf::Validate(path);
  CHECK(r.ok);
  CHECK_EQ(r.declared_pages, 1u);
  CHECK_EQ(r.width_px, kW);
  CHECK_EQ(r.height_px, kH);
}
```

- [ ] **Step 2: 跑测试**

```bash
app/shared/urf/run_tests.sh
```

期望：`全部通过`，35 个测试用例。整个测试跑完应在几秒内。

- [ ] **Step 3: 提交**

```bash
git add app/shared/urf
git commit -m "test(urf): A4 整页按条带写入的回归测试"
```

---

## Task 12: README 与交接

**Files:**
- Create: `app/shared/urf/README.md`

- [ ] **Step 1: 写 README**

`app/shared/urf/README.md`：

```markdown
# URF 编码器

跨平台纯 C++，无平台依赖，供 iOS / Android 的 RasterKit TurboModule 共用。

## 跑测试

    ./run_tests.sh

不需要手机，不需要打印机。这是这个项目里唯一能在不烧纸的前提下验证光栅正确性的手段。

## 用法

    urf::PageSpec spec;
    spec.width_px  = 4962;   // 来自 GET /api/device/{dev}/render-profile
    spec.height_px = 7014;
    spec.dpi       = 600;

    urf::Writer w("/tmp/job.urf");
    w.BeginPage(spec);
    while (还有条带) {
      渲染一带到 band（每行 spec.width_px 字节的 8 位灰度）;
      w.WriteRows(band, 本带行数);
    }
    w.EndPage();
    w.Close();                // 这一步才回填真实页数

    auto r = urf::ValidateForUpload("/tmp/job.urf", 4962, 7014);
    if (!r.ok) 中止上传，把 r.error 给用户;

峰值内存由条带高度决定，与页面尺寸无关。A4 600dpi 整页是 34.8MB，
不要构造整页缓冲。

## 三个容易写错的地方

1. **页数字段**。文件头第 8~11 字节，大端。写 0 打印机认为文档为空，什么都不打。
   `Writer` 先写占位 0，`Close()` 时 seek 回去回填——所以**不 Close 就上传等于废纸**。
2. **字面串长度不能是 1**。编码是 `257-len`，`len=1` 溢出成 256。单个不重复像素
   必须退回游程编码。见 `packbits.cpp` 里那个 `if (len == 1)`。
3. **页头常量还没在真机核对过**。见 `HEADER-FIELDS.md`。第一次真机打印前必须核。

## 格式依据

字节布局全部来自 `tools/reference/render.py` 的 `fix_page_count`（真实跑通过的
扫描器）和 `docs/API-cloud-print.md` 第 7 节的最小样本。不是从网上抄的。
```

- [ ] **Step 2: 提交**

```bash
git add app/shared/urf/README.md
git commit -m "docs(urf): 编码器 README"
```

---

## 完成标准

- [ ] `app/shared/urf/run_tests.sh` 全绿，35 个测试用例
- [ ] `python3 app/tools/urfdump.py /tmp/urf_a4.urf` 输出 `尺寸=4962x7014 dpi=600`、`实际页数 1`
- [ ] `HEADER-FIELDS.md` 里每个字段都有明确的核对状态，没有含糊其辞的项

## 明确不在本计划内

下面这些属于阶段二、三，各自有独立计划：

- RN 工程骨架、API client、mock server、账号
- iOS / Android 的 PDF 渲染与 TurboModule 桥接
- 图片源的 Floyd–Steinberg 抖动（发生在喂给编码器之前，不属于编码器）
- 上传
