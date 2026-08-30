#include "urf/urf.h"

namespace urf {

namespace {

constexpr uint32_t kMaxDimension = 30000;

// 带缓冲的顺序读。
//
// 早先的实现把整个文件读进 std::vector 再扫，在 Android 真机上测出峰值常驻
// 内存 71MB——A4 整页产物 35MB，vector 扩容翻倍就是 70MB。而作业上限是
// 200MB（API 文档 4.4），照那个写法上传前自校验会先把手机打死。
// 校验器只需要顺序前进，不需要随机访问，所以流式读就够。
class ByteSource {
 public:
  explicit ByteSource(std::FILE* fp) : fp_(fp) {}

  // 读一个字节。到文件尾返回 false。
  bool Read(uint8_t* out) {
    if (pos_ == len_ && !Refill()) return false;
    *out = buf_[pos_++];
    return true;
  }

  // 读满 n 个字节。不够返回 false。
  bool ReadExact(uint8_t* dst, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      if (!Read(dst + i)) return false;
    }
    return true;
  }

  // 跳过 n 个字节。不够返回 false。
  bool Skip(size_t n) {
    while (n > 0) {
      if (pos_ == len_ && !Refill()) return false;
      size_t avail = len_ - pos_;
      size_t take = (n < avail) ? n : avail;
      pos_ += take;
      n -= take;
    }
    return true;
  }

 private:
  bool Refill() {
    len_ = std::fread(buf_, 1, sizeof buf_, fp_);
    pos_ = 0;
    return len_ > 0;
  }

  std::FILE* fp_;
  uint8_t buf_[65536];
  size_t pos_ = 0;
  size_t len_ = 0;
};

uint32_t ReadBE32(const uint8_t* d) {
  return (static_cast<uint32_t>(d[0]) << 24) |
         (static_cast<uint32_t>(d[1]) << 16) |
         (static_cast<uint32_t>(d[2]) << 8) |
         static_cast<uint32_t>(d[3]);
}

}  // namespace

ValidateResult Validate(const std::string& path) {
  ValidateResult r;

  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) { r.error = "打不开文件: " + path; return r; }
  ByteSource src(f);

  uint8_t head[12];
  if (!src.ReadExact(head, sizeof head)) {
    std::fclose(f);
    r.error = "文件不足 12 字节，连文件头都不够";
    return r;
  }

  static const uint8_t kMagic[8] = {'U', 'N', 'I', 'R', 'A', 'S', 'T', 0x00};
  for (int i = 0; i < 8; ++i) {
    if (head[i] != kMagic[i]) {
      std::fclose(f);
      r.error = "魔数不匹配，期望 UNIRAST\\0";
      return r;
    }
  }

  r.declared_pages = ReadBE32(head + 8);
  if (r.declared_pages == 0) {
    std::fclose(f);
    r.error = "页数字段为 0，打印机会认为文档为空";
    return r;
  }

  // 逐页扫描。规则与 tools/reference/render.py 的 fix_page_count 一致。
  for (;;) {
    uint8_t ph[32];
    if (!src.ReadExact(ph, sizeof ph)) break;   // 没有下一页了

    uint32_t w = ReadBE32(ph + 12);
    uint32_t h = ReadBE32(ph + 16);
    if (w == 0 || w >= kMaxDimension || h == 0 || h >= kMaxDimension) break;
    if (r.actual_pages == 0) { r.width_px = w; r.height_px = h; }
    ++r.actual_pages;

    // 行的结束是「像素数攒够 width」，不是某个终止字节。
    // 重复计数 N 表示该行共出现 N+1 次。两条都由 tests/fixtures/ 的真实样本确定。
    uint32_t rows = 0;
    bool truncated = false;
    while (rows < h) {
      uint8_t repeat;
      if (!src.Read(&repeat)) { truncated = true; break; }

      uint32_t px = 0;
      while (px < w) {
        uint8_t c;
        if (!src.Read(&c)) { truncated = true; break; }
        uint32_t count;
        size_t skip;
        if (c < 128) {
          count = static_cast<uint32_t>(c) + 1;   // 重复下一个像素 c+1 次
          skip = 1;
        } else if (c == 128) {
          truncated = true;                        // 128 不是合法包首字节
          break;
        } else {
          count = static_cast<uint32_t>(257 - c);  // 接 257-c 个字面像素
          skip = count;
        }
        px += count;
        if (!src.Skip(skip)) { truncated = true; break; }
      }
      if (truncated || px != w) { truncated = true; break; }

      rows += static_cast<uint32_t>(repeat) + 1;
      if (rows > h) { truncated = true; break; }
    }
    if (rows != h || truncated) {
      std::fclose(f);
      r.error = "第 " + std::to_string(r.actual_pages) + " 页的行数据不完整";
      return r;
    }
  }
  std::fclose(f);

  if (r.actual_pages == 0) { r.error = "扫不出任何一页，首页尺寸非法"; return r; }
  if (r.actual_pages != r.declared_pages) {
    r.error = "页数字段是 " + std::to_string(r.declared_pages) +
              "，实际扫出 " + std::to_string(r.actual_pages) + " 页";
    return r;
  }

  r.ok = true;
  return r;
}

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

}  // namespace urf
