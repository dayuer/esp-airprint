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
