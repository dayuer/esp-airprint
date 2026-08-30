#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace urf {

// 把一行 8 位灰度像素编码成 URF 的行数据，追加到 out。
//
// 输出不含前置的「行重复计数」字节，**也不含任何行结束符**——URF 的行以
// 「已产出的像素数攒够 width」结束，没有终止字节。这一点由
// tests/fixtures/ 里 Apple 光栅器的真实产物确定。曾经按 0x80 断行的模型是错的，
// 多写一个 0x80 会被解码成 129 个字面像素，整页从第一行起全烂。
void EncodeRowGray8(const uint8_t* row, size_t width, std::vector<uint8_t>& out);

// colorspace 字段取值。数值待 Task 9 用真实 cupsfilter 产物核对。
// 页头常量。取值由 tests/fixtures/ 里 Apple 光栅器的真实产物核对过。
constexpr uint8_t kColorspaceGray = 0;
// URF 的 duplex 枚举：1 = 单面。0 不是合法值——Apple 的产物写的是 1。
constexpr uint8_t kDuplexOneSided = 1;

struct PageSpec {
  uint32_t width_px = 0;
  uint32_t height_px = 0;
  uint32_t dpi = 0;
  uint8_t bits_per_pixel = 8;
  uint8_t colorspace = kColorspaceGray;
  uint8_t duplex = kDuplexOneSided;
};

// 12 字节文件头，页数先写占位值。
void WriteFileHeader(std::vector<uint8_t>& out, uint32_t page_count);

// 32 字节页头。
void WritePageHeader(std::vector<uint8_t>& out, const PageSpec& spec);

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
  // line_repeat 把连续相同的行合并成一个行重复计数（值 N 表示该行共出现 N+1 次）。
  //
  // 默认打开。这不是可选优化：Apple 的产物里一页 3507 行只用了 100 条记录，
  // 10KB；每行独立编码是 147KB。语义已由 tests/fixtures/ 的真实样本核对。
  explicit Writer(const std::string& path, bool line_repeat = true);
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
  bool line_repeat_ = false;
  std::vector<uint8_t> pending_;      // 上一行的编码结果（不含重复计数）
  uint32_t pending_count_ = 0;        // 上一行重复了几次，0 表示没有待写的行
  void FlushPending();
};

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

// 在上传之前调用。除了整份自洽，还要求首页尺寸等于 render-profile 给的值。
//
// 这道断言的意义：URF 是按某台打印机的 dpi 和像素尺寸光栅的，尺寸错了服务端会
// 返回 400，漏过去就是错位或半页。而服务端不解析文档、设备不认识格式，
// 没有任何环节会发现——所以本地必须自己拦。
ValidateResult ValidateForUpload(const std::string& path,
                                 uint32_t expect_width_px,
                                 uint32_t expect_height_px);

}  // namespace urf
