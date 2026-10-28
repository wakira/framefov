// Tiny self-contained unit test framework (no external dependency).
#pragma once

#include <cmath>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace framefovtest {

struct TestCase {
  std::string name;
  void (*fn)();
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

struct Failure : std::exception {
  std::string msg;
  explicit Failure(std::string m) : msg(std::move(m)) {}
  const char* what() const noexcept override { return msg.c_str(); }
};

inline std::string where(const char* file, int line) {
  std::string s(file ? file : "?");
  size_t slash = s.find_last_of('/');
  if (slash != std::string::npos) s = s.substr(slash + 1);
  return s + ":" + std::to_string(line);
}

template <class A, class B>
inline void checkEq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
  // Comparing e.g. a size_t with an int literal is intentional in tests.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
  const bool equal = (a == b);
#pragma GCC diagnostic pop
  if (!equal) {
    throw Failure(where(file, line) + ": CHECK_EQ failed: " + ea + " == " + eb);
  }
}

inline void checkNear(double a, double b, double eps, const char* ea, const char* eb, const char* file,
                      int line) {
  if (!(std::fabs(a - b) <= eps)) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: CHECK_NEAR failed: %s (%g) vs %s (%g), eps %g",
             where(file, line).c_str(), ea, a, eb, b, eps);
    throw Failure(buf);
  }
}

inline int runAll(const char* filter) {
  int failed = 0, ran = 0;
  for (const auto& t : registry()) {
    if (filter && t.name.find(filter) == std::string::npos) continue;
    ++ran;
    try {
      t.fn();
      printf("[ ok ] %s\n", t.name.c_str());
    } catch (const Failure& f) {
      printf("[FAIL] %s\n        %s\n", t.name.c_str(), f.what());
      ++failed;
    } catch (const std::exception& e) {
      printf("[FAIL] %s\n        unexpected exception: %s\n", t.name.c_str(), e.what());
      ++failed;
    }
  }
  printf("%d test(s) ran, %d failed\n", ran, failed);
  return failed == 0 ? 0 : 1;
}

}  // namespace framefovtest

#define FRAMEFOV_TEST(name)                                          \
  static void name();                                             \
  static ::framefovtest::Registrar framefov_test_reg_##name(#name, name); \
  static void name()

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      throw ::framefovtest::Failure(::framefovtest::where(__FILE__, __LINE__) +        \
                                 ": CHECK failed: " #cond);                      \
    }                                                                            \
  } while (0)

#define CHECK_EQ(a, b) ::framefovtest::checkEq((a), (b), #a, #b, __FILE__, __LINE__)

#define CHECK_NEAR(a, b, eps) ::framefovtest::checkNear((a), (b), (eps), #a, #b, __FILE__, __LINE__)
