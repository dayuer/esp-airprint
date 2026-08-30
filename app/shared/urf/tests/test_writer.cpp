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
