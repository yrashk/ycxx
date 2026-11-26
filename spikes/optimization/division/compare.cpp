import std;
import ycxx.bench;

using Word = unsigned long long;
using namespace ycxx::bench;
extern "C" Word plain_shard_index(Word, Word, Word);
extern "C" Word fused_shard_index(Word, Word, Word);
extern "C" Word plain_lane_index(unsigned, unsigned, unsigned);
extern "C" Word fused_lane_index(unsigned, unsigned, unsigned);
extern "C" Word fused_page_offset(Word, Word);
extern "C" Word plain_page_offset(Word, Word);

Word advance(Word x) { return x * 6364136223846793005ULL + 1442695040888963407ULL; }

struct Shard {
  Word (*fn)(Word, Word, Word);
  Word operator()(Word x, Word salt) const { return fn(x, (x >> 41) + 1, salt); }
};
struct Lane {
  Word (*fn)(unsigned, unsigned, unsigned);
  Word operator()(Word x, Word salt) const {
    return fn(static_cast<unsigned>(x), static_cast<unsigned>(x >> 48) + 1, static_cast<unsigned>(salt));
  }
};

template<class Fn>
Word batch(Fn fn, Iterations iterations) {
  Word x = 1;
  Word salt = 0;
  for (Iterations i = 0; i != iterations; ++i) {
    x = advance(x);
    salt = fn(x, salt);
  }
  return salt;
}

template<class Left, class Right>
void compare(std::string_view left_name, std::string_view right_name, Left left, Right right,
             const Configuration& config) {
  for (const Word x : {0ULL, 1ULL, (1ULL << 63) - 1, (1ULL << 63), ~0ULL})
    if (left(x, advance(x)) != right(x, advance(x))) throw std::runtime_error("edge mismatch");
  Word x = 0;
  for (int i = 0; i < 100000; ++i) {
    const Word salt = advance(x);
    if (left(x, salt) != right(x, salt)) throw std::runtime_error("workload mismatch");
    x = salt;
  }
  const auto data = compare_batched(
      [&](Iterations n) { return batch(left, n); },
      [&](Iterations n) { return batch(right, n); }, config);
  write_ndjson(std::cout, left_name, right_name, data);
}

int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && std::string_view(argv[1]) != "--deep")) {
    std::println(std::cerr, "usage: division_compare [--deep]");
    return 2;
  }
  auto config = configuration_for(argc == 2 ? Band::deep : Band::standard);
  config.process_telemetry = true;
  compare("two DIV · unsigned 64-bit shard index", "one DIV · unsigned 64-bit shard index",
          Shard{plain_shard_index}, Shard{fused_shard_index}, config);
  compare("two DIV · unsigned 32-bit lane index", "one DIV · unsigned 32-bit lane index",
          Lane{plain_lane_index}, Lane{fused_lane_index}, config);
  compare("heuristic · power-of-two page offset", "solver · power-of-two page offset",
          plain_page_offset, fused_page_offset, config);
}
