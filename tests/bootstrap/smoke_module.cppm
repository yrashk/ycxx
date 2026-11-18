// A named module with an exported template, a non-exported helper and a partition-free
// interface, imported by every smoke program.
export module bootstrap.smoke;

import std;

int failures = 0;

export namespace smoke {
void check(bool ok, std::string_view what, std::source_location at = std::source_location::current()) {
  if (!ok) {
    ++failures;
    std::println(std::cerr, "{}:{}: FAIL: {}", at.file_name(), at.line(), what);
  }
}

int result(std::string_view feature) {
  std::println("{}: {}", feature, failures == 0 ? "ok" : "FAIL");
  return failures == 0 ? 0 : 1;
}

template <class T>
concept summable = requires(T a, T b) { { a + b } -> std::convertible_to<T>; };

template <summable T>
constexpr T sum(std::span<const T> xs) {
  return std::ranges::fold_left(xs, T{}, std::plus{});
}
}  // namespace smoke
