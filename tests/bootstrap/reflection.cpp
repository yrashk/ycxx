// Reflection (P2996) as ycxx uses it in place of boilerplate: member-wise hashing and equality
// of query keys, enumerator names for option parsing and diagnostics tables, and generated
// aggregates (define_aggregate) for node definitions.
import std;
import bootstrap.smoke;

namespace {

struct key {
  std::uint32_t kind;
  std::uint64_t item;
  std::string name;
};

// FNV-1a over the members, in declaration order, with no per-type code.
template <class T>
std::uint64_t hash(const T& v) {
  std::uint64_t h = 1469598103934665603u;
  auto mix = [&](std::span<const std::byte> bytes) {
    for (auto b : bytes) h = (h ^ std::to_integer<std::uint64_t>(b)) * 1099511628211u;
  };
  constexpr auto ctx = std::meta::access_context::unchecked();
  template for (constexpr auto m : std::define_static_array(std::meta::nonstatic_data_members_of(^^T, ctx))) {
    const auto& field = v.[:m:];
    if constexpr (requires { std::as_bytes(std::span(field)); }) {
      mix(std::as_bytes(std::span(field)));
    } else {
      mix(std::as_bytes(std::span(&field, 1)));
    }
  }
  return h;
}

template <class T>
bool equal(const T& a, const T& b) {
  constexpr auto ctx = std::meta::access_context::unchecked();
  template for (constexpr auto m : std::define_static_array(std::meta::nonstatic_data_members_of(^^T, ctx))) {
    if (a.[:m:] != b.[:m:]) return false;
  }
  return true;
}

enum class severity { note, warning, error };

template <class E>
  requires std::is_enum_v<E>
constexpr std::string_view name_of(E e) {
  template for (constexpr auto x : std::define_static_array(std::meta::enumerators_of(^^E))) {
    if (e == [:x:]) return std::meta::identifier_of(x);
  }
  return "<unknown>";
}

struct generated;
consteval {
  std::meta::define_aggregate(^^generated, {
    std::meta::data_member_spec(^^int, {.name = "line"}),
    std::meta::data_member_spec(^^int, {.name = "column"}),
  });
}

}  // namespace

int main() {
  key a{1, 42, "f"}, b{1, 42, "f"}, c{1, 43, "f"};
  smoke::check(hash(a) == hash(b), "equal keys hash equal");
  smoke::check(hash(a) != hash(c), "different keys hash differently");
  smoke::check(equal(a, b) && !equal(a, c), "member-wise equality");
  static_assert(name_of(severity::warning) == "warning");
  smoke::check(name_of(severity::error) == "error", "enumerator names at run time");
  generated g{.line = 3, .column = 7};
  smoke::check(g.line == 3 && g.column == 7, "define_aggregate");
  static_assert(std::meta::nonstatic_data_members_of(^^generated, std::meta::access_context::current()).size() == 2);
  return smoke::result("reflection");
}
