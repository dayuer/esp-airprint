#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace urf {

// 把一行 8 位灰度像素编码成 URF 的行数据，追加到 out。
// 输出不含前置的「行重复计数」字节，但**含**行尾的 0x80。
// width 为 0 时只追加 0x80。
void EncodeRowGray8(const uint8_t* row, size_t width, std::vector<uint8_t>& out);

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
  // line_repeat 打开后，连续相同的行会合并成一个行重复计数。
  // 默认关闭：这个字节的语义还没在真打印机上验过，见 HEADER-FIELDS.md。
  explicit Writer(const std::string& path, bool line_repeat = false);
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

}  // namespace urf
