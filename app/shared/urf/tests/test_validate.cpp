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
  Bytes d = {'U', 'N', 'I', 'R', 'A', 'S', 'T', 0x00, 0x00, 0x00, 0x00, 0x01};
  for (int i = 0; i < 12; ++i) d.push_back(0);
  Bytes wh = {0x00, 0x00, 0x13, 0x62, 0x00, 0x00, 0x1B, 0x66};  // 4962 x 7014
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
  d[11] = 0;
  WriteRaw(path, d);
  urf::ValidateResult r = urf::Validate(path);
  CHECK(!r.ok);
  CHECK(r.error.find("页数") != std::string::npos);
}

TEST(尺寸越界要报错) {
  const std::string path = "/tmp/urf_bigdim.urf";
  Bytes d = MinimalUrf();
  d[12 + 12] = 0xFF;
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

static std::string MakeOnePageFile(uint32_t w, uint32_t h) {
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
  std::string p = MakeOnePageFile(120, 30);
  urf::ValidateResult r = urf::ValidateForUpload(p, 120, 30);
  CHECK(r.ok);
}

TEST(宽度不一致时拒绝且报出期望值) {
  std::string p = MakeOnePageFile(120, 30);
  urf::ValidateResult r = urf::ValidateForUpload(p, 4962, 30);
  CHECK(!r.ok);
  CHECK(r.error.find("4962") != std::string::npos);
  CHECK(r.error.find("120") != std::string::npos);
}

TEST(高度不一致时拒绝) {
  std::string p = MakeOnePageFile(120, 30);
  urf::ValidateResult r = urf::ValidateForUpload(p, 120, 7014);
  CHECK(!r.ok);
}
