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
