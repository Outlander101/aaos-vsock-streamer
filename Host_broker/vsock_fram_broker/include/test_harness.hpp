
#pragma once
#include <cstddef>  // std::size_t
#include <cstdio>
#include <cstdlib>
#include <new>  // placement new (fixes operator new(..., void*))
#include <string>
#include <vector>

struct TestCase {
  const char* name;
  void (*fn)();
};

std::vector<TestCase>& test_registry();

struct Registrar {
  Registrar(const char* n, void (*f)());
};

#define TEST(name)                           \
  static void name();                        \
  static Registrar reg_##name(#name, &name); \
  static void name()

#define ASSERT_TRUE(x)                                                        \
  do {                                                                        \
    if (!(x)) {                                                               \
      std::printf("[FAIL] %s:%d: ASSERT_TRUE(%s)\n", __FILE__, __LINE__, #x); \
      std::abort();                                                           \
    }                                                                         \
  } while (0)

#define ASSERT_FALSE(x) ASSERT_TRUE(!(x))

#define ASSERT_EQ(a, b)                                                      \
  do {                                                                       \
    auto _a = (a);                                                           \
    auto _b = (b);                                                           \
    if (!((_a) == (_b))) {                                                   \
      std::printf("[FAIL] %s:%d: ASSERT_EQ(%s,%s) got (%lld) vs (%lld)\n",   \
                  __FILE__, __LINE__, #a, #b, (long long)_a, (long long)_b); \
      std::abort();                                                          \
    }                                                                        \
  } while (0)

#define ASSERT_NE(a, b)                                                       \
  do {                                                                        \
    auto _a = (a);                                                            \
    auto _b = (b);                                                            \
    if (((_a) == (_b))) {                                                     \
      std::printf("[FAIL] %s:%d: ASSERT_NE(%s,%s)\n", __FILE__, __LINE__, #a, \
                  #b);                                                        \
      std::abort();                                                           \
    }                                                                         \
  } while (0)

#define ASSERT_STR_EQ(a, b)                                                   \
  do {                                                                        \
    std::string _a = (a);                                                     \
    std::string _b = (b);                                                     \
    if (!(_a == _b)) {                                                        \
      std::printf("[FAIL] %s:%d: ASSERT_STR_EQ got '%s' vs '%s'\n", __FILE__, \
                  __LINE__, _a.c_str(), _b.c_str());                          \
      std::abort();                                                           \
    }                                                                         \
  } while (0)
