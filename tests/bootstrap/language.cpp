// Other C++23/26 features DECISIONS §3.2 names: deducing this, pack indexing, structured-binding
// packs, constexpr exceptions, std::expected, #embed.
import std;
import bootstrap.smoke;

namespace {

struct node {
  int value;
  std::vector<node> children;
  // Deducing this: one recursive visitor, no const/non-const pair.
  template <class Self>
  int total(this Self&& self) {
    return self.value + std::ranges::fold_left(self.children | std::views::transform([](auto& c) { return c.total(); }), 0,
                                               std::plus{});
  }
};

template <class... Ts>
constexpr auto last(Ts... xs) {
  return xs...[sizeof...(Ts) - 1];
}

constexpr int sum_fields(auto aggregate) {
  auto [... xs] = aggregate;
  return (0 + ... + xs);
}

constexpr int parse_digit(char c) {
  if (c < '0' || c > '9') throw std::invalid_argument("not a digit");
  return c - '0';
}

constexpr bool rejects_letter() {
  try {
    parse_digit('x');
  } catch (const std::invalid_argument&) {
    return true;
  }
  return false;
}

std::expected<int, std::string> parse(std::string_view s) {
  int v = 0;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || ptr != s.data() + s.size()) return std::unexpected(std::format("bad number '{}'", s));
  return v;
}

constexpr unsigned char embedded[] = {
#embed "embed.txt"
};

struct point {
  int x, y, z;
};

}  // namespace

int main() {
  node tree{1, {{2, {}}, {3, {{4, {}}}}}};
  smoke::check(tree.total() == 10, "deducing this");
  static_assert(last(1, 'a', 2.5) == 2.5);
  static_assert(sum_fields(point{1, 2, 3}) == 6);
  static_assert(rejects_letter(), "constexpr exceptions");
  smoke::check(parse("42") == 42 && !parse("4x"), "std::expected");
  static_assert(sizeof embedded == 14 && embedded[0] == 'e' && embedded[13] == '\n', "#embed");
  return smoke::result("language");
}
