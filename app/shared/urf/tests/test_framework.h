#pragma once
// 极简测试框架：不引入 Catch2/gtest，避免为了跑一个编码器单测而联网拉依赖。
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace urftest {

struct Case { std::string name; std::function<void()> fn; };

std::vector<Case>& registry();
int& failures();
void fail(const char* file, int line, const std::string& msg);
std::string hex(const std::vector<unsigned char>& v);

// 用一个构造函数做注册，避免 -Wunused-const-variable。
struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

}  // namespace urftest

#define TEST(name)                                                        \
  static void name();                                                     \
  static const urftest::Registrar name##_reg(#name, name);                \
  static void name()

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) urftest::fail(__FILE__, __LINE__, "CHECK(" #cond ")");   \
  } while (0)

#define CHECK_EQ(a, b)                                                    \
  do {                                                                    \
    auto _a = (a); auto _b = (b);                                         \
    if (!(_a == _b))                                                      \
      urftest::fail(__FILE__, __LINE__,                                   \
                    "CHECK_EQ(" #a ", " #b ") 左=" + std::to_string(_a) + \
                        " 右=" + std::to_string(_b));                     \
  } while (0)

#define CHECK_BYTES(actual, ...)                                          \
  do {                                                                    \
    std::vector<unsigned char> _exp = __VA_ARGS__;                        \
    std::vector<unsigned char> _act = (actual);                           \
    if (_act != _exp)                                                     \
      urftest::fail(__FILE__, __LINE__,                                   \
                    "字节不符\n    实际: " + urftest::hex(_act) +          \
                        "\n    期望: " + urftest::hex(_exp));              \
  } while (0)
