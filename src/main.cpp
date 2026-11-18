import std;
import ycxx.driver;

int main(int argc, char** argv) {
  return ycxx::driver::run(std::span(argv, static_cast<std::size_t>(argc)) |
                           std::views::transform([](const char* a) { return std::string_view(a); }) |
                           std::ranges::to<std::vector>());
}
