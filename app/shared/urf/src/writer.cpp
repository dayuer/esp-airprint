#include "urf/urf.h"

namespace urf {

namespace {

constexpr uint32_t kMaxDimension = 30000;   // 与服务端入口校验一致

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
  out.push_back(spec.duplex);
  out.push_back(0);   // quality
  for (int i = 0; i < 8; ++i) out.push_back(0);
  PushBE32(out, spec.width_px);
  PushBE32(out, spec.height_px);
  PushBE32(out, spec.dpi);
  for (int i = 0; i < 8; ++i) out.push_back(0);
}

Writer::Writer(const std::string& path, bool line_repeat)
    : line_repeat_(line_repeat) {
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

}  // namespace urf
