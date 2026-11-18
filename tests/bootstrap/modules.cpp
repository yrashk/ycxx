// Modules: `import std;` from libycxx and a named module of our own.
import std;
import bootstrap.smoke;

int main() {
  constexpr std::array xs{1, 2, 3, 4};
  static_assert(smoke::sum<int>(xs) == 10);
  std::vector<std::string> words{"one", "two", "three"};
  smoke::check(std::ranges::to<std::string>(words | std::views::join_with(',')) == "one,two,three",
               "views::join_with through import std");
  smoke::check(std::format("{:>5}", 42) == "   42", "std::format through import std");
  std::map<std::string, int> m{{"a", 1}};
  smoke::check(m.at("a") == 1, "std::map through import std");
  return smoke::result("modules");
}
