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
  CHECK_EQ(Encode({}).size(), static_cast<size_t>(0));
}

// 单个像素编码成「游程 1」：n=0 表示重复 0+1 次。
TEST(单像素) {
  CHECK_BYTES(Encode({0x41}), Bytes{0x00, 0x41});
}

// 3 个相同像素：n = 3-1 = 2。
TEST(短游程) {
  CHECK_BYTES(Encode({0xAA, 0xAA, 0xAA}), Bytes{0x02, 0xAA});
}

// 游程上限是 128（n=127）。129 个像素要拆成 128 + 1。
TEST(游程上限) {
  Bytes row(129, 0xFF);
  CHECK_BYTES(Encode(row), Bytes{0x7F, 0xFF, 0x00, 0xFF});
}

// A4 600dpi 一整行白：4962 = 128*38 + 98。
TEST(整行白) {
  Bytes row(4962, 0xFF);
  Bytes got = Encode(row);
  Bytes want;
  for (int i = 0; i < 38; ++i) { want.push_back(0x7F); want.push_back(0xFF); }
  want.push_back(97);   // 98-1
  want.push_back(0xFF);
  CHECK_BYTES(got, want);
}

// 两个不同像素编码成字面串：len=2 → n = 257-2 = 255。
TEST(短字面串) {
  CHECK_BYTES(Encode({0x00, 0xFF}), Bytes{0xFF, 0x00, 0xFF});
}

// 游程在前，字面串在后。
TEST(游程加字面串) {
  CHECK_BYTES(Encode({0xAA, 0xAA, 0xAA, 0x01, 0x02}),
              Bytes{0x02, 0xAA, 0xFF, 0x01, 0x02});
}

// 单个孤立像素后面跟着游程：字面长度会是 1，必须退回游程编码，
// 否则 257-1=256 溢出成 0x00，解码端会把它当成「重复 1 次」而错位。
TEST(孤立像素退回游程) {
  CHECK_BYTES(Encode({0x01, 0x02, 0x02}),
              Bytes{0x00, 0x01, 0x01, 0x02});
}

// 字面串上限 128：n = 257-128 = 129。
TEST(字面串上限) {
  Bytes row;
  for (int i = 0; i < 130; ++i) row.push_back(static_cast<unsigned char>(i));
  Bytes got = Encode(row);
  CHECK_EQ(static_cast<int>(got[0]), 129);
  CHECK_EQ(static_cast<int>(got[1]), 0);
  CHECK_EQ(static_cast<int>(got[128]), 127);
  CHECK_EQ(static_cast<int>(got[129]), 255);   // 257-2
  CHECK_EQ(static_cast<int>(got[130]), 128);
  CHECK_EQ(static_cast<int>(got[131]), 129);
  CHECK_EQ(got.size(), static_cast<size_t>(132));
}

// 逐字节检查一段确定的数据能被完整解码回原样。
TEST(往返一致) {
  Bytes row;
  unsigned s = 12345;
  for (int i = 0; i < 1000; ++i) {
    s = s * 1103515245u + 12345u;
    row.push_back(static_cast<unsigned char>((s >> 16) % 4));
  }
  Bytes enc = Encode(row);

  // 按真实 URF 的规则解码：攒够 width 个像素即为一行，没有终止字节。
  Bytes dec;
  size_t p = 0;
  while (dec.size() < row.size() && p < enc.size()) {
    unsigned char n = enc[p++];
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
