#include "urf/urf.h"
#include <cstdio>
#include <string>
#include <vector>

using Bytes = std::vector<unsigned char>;

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

TEST(文件头是12字节且页数大端) {
  std::vector<uint8_t> out;
  urf::WriteFileHeader(out, 1);
  CHECK_EQ(out.size(), static_cast<size_t>(12));
  CHECK_BYTES(Bytes(out.begin(), out.begin() + 8),
              Bytes{'U', 'N', 'I', 'R', 'A', 'S', 'T', 0x00});
  CHECK_BYTES(Bytes(out.begin() + 8, out.end()), Bytes{0x00, 0x00, 0x00, 0x01});
}

TEST(页数258验证大端不是小端) {
  std::vector<uint8_t> out;
  urf::WriteFileHeader(out, 258);
  CHECK_BYTES(Bytes(out.begin() + 8, out.end()), Bytes{0x00, 0x00, 0x01, 0x02});
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
  CHECK_BYTES(Bytes(out.begin() + 12, out.begin() + 16), Bytes{0x00, 0x00, 0x13, 0x62});
  // 7014 = 0x00001B66
  CHECK_BYTES(Bytes(out.begin() + 16, out.begin() + 20), Bytes{0x00, 0x00, 0x1B, 0x66});
  // 600 = 0x00000258
  CHECK_BYTES(Bytes(out.begin() + 20, out.begin() + 24), Bytes{0x00, 0x00, 0x02, 0x58});
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

TEST(写一页并回填页数) {
  const std::string path = "/tmp/urf_test_one.urf";
  {
    urf::Writer w(path);
    w.BeginPage(SmallSpec(4, 2));
    std::vector<uint8_t> rows = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
    w.WriteRows(rows.data(), 2);
    w.EndPage();
    w.Close();
    CHECK_EQ(w.pages(), 1u);
  }
  Bytes d = ReadFile(path);
  CHECK_BYTES(Bytes(d.begin() + 8, d.begin() + 12), Bytes{0, 0, 0, 1});
  CHECK_BYTES(Bytes(d.begin() + 44, d.begin() + 48), Bytes{0x00, 0x03, 0xFF, 0x80});
  CHECK_BYTES(Bytes(d.begin() + 48, d.begin() + 52), Bytes{0x00, 0x03, 0x00, 0x80});
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
    w.WriteRows(band.data(), 2);
    w.WriteRows(band.data(), 2);
    w.EndPage();
    w.Close();
  }
  Bytes d = ReadFile(path);
  CHECK_BYTES(Bytes(d.begin() + 8, d.begin() + 12), Bytes{0, 0, 0, 1});
  CHECK_EQ(d.size(), static_cast<size_t>(12 + 32 + 16));
}

TEST(行数不足时EndPage要抛) {
  bool threw = false;
  try {
    urf::Writer w("/tmp/urf_test_short.urf");
    w.BeginPage(SmallSpec(2, 4));
    std::vector<uint8_t> band = {0x01, 0x01};
    w.WriteRows(band.data(), 1);
    w.EndPage();
  } catch (const std::runtime_error&) { threw = true; }
  CHECK(threw);
}

TEST(行数超出时WriteRows要抛) {
  bool threw = false;
  try {
    urf::Writer w("/tmp/urf_test_over.urf");
    w.BeginPage(SmallSpec(2, 1));
    std::vector<uint8_t> band = {0x01, 0x01, 0x02, 0x02};
    w.WriteRows(band.data(), 2);
  } catch (const std::runtime_error&) { threw = true; }
  CHECK(threw);
}

TEST(零尺寸页要抛) {
  bool threw = false;
  try {
    urf::Writer w("/tmp/urf_test_zero.urf");
    w.BeginPage(SmallSpec(0, 10));
  } catch (const std::runtime_error&) { threw = true; }
  CHECK(threw);
}

TEST(一页都没有就Close要抛) {
  bool threw = false;
  try {
    urf::Writer w("/tmp/urf_test_empty.urf");
    w.Close();   // 页数 0 会让打印机认为文档为空
  } catch (const std::runtime_error&) { threw = true; }
  CHECK(threw);
}

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
    std::vector<uint8_t> band = {0x11, 0x11, 0x11, 0x11, 0x22, 0x22};
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
