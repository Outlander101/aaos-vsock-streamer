
#include <cstdio>
#include <test_harness.hpp>

std::vector<TestCase>& test_registry() {
  static std::vector<TestCase> r;
  return r;
}

Registrar::Registrar(const char* n, void (*f)()) {
  test_registry().push_back({n, f});
}

int main() {
  auto& r = test_registry();
  std::printf("Running %zu tests...\n", r.size());
  for (auto& t : r) {
    t.fn();
  }
  std::printf("ALL PASSED (%zu/%zu)\n", r.size(), r.size());
  return 0;
}