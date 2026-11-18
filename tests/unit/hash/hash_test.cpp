// ycxx.hash: MurmurHash3_x64_128 against published-algorithm vectors, streaming against one-shot,
// constant evaluation against run time, and the reflection-derived structural fingerprint.
import std;
import ycxx.hash;
import ycxx.unit;

using ycxx::Fp128;
using ycxx::unit::check;

namespace {

// Vectors from an independent implementation of MurmurHash3_x64_128 (the Python `mmh3` package,
// `hash128(s, seed, x64arch=True)`), with the 32-bit seed in both halves of the state.
struct vector_case {
  std::string_view in;
  std::uint32_t seed;
  Fp128 out;
};
constexpr vector_case vectors[] = {
    {"", 0, {0x0, 0x0}},
    {"", 0x9747b28c, {0x392b208a1daabbb3, 0x93b0608fe302957a}},
    {"a", 0, {0x85555565f6597889, 0xe6b53a48510e895a}},
    {"a", 0x9747b28c, {0x5ce8d8512db25a1d, 0x9e6dab0f9208f004}},
    {"abc", 0, {0xb4963f3f3fad7867, 0x3ba2744126ca2d52}},
    {"abc", 0x9747b28c, {0x3743630dbfc3cedc, 0xcde0a23420b504bf}},
    {"hello, world", 0, {0x342fac623a5ebc8e, 0x4cdcbc079642414d}},
    {"hello, world", 0x9747b28c, {0xe6723010086c5b1e, 0x2f3637061e4d8932}},
    {"0123456789abcdef", 0, {0x4be06d94cf4ad1a7, 0x87c35b5c63a708da}},
    {"0123456789abcdef", 0x9747b28c, {0xd8061eb14f48c9c9, 0x521e1174e459c2cc}},
    {"0123456789abcdef0", 0, {0xeb24ae8785a5c075, 0x73fb68b3313128ca}},
    {"0123456789abcdef0", 0x9747b28c, {0x993262a7b6af8d70, 0xc738cec6e32ab89c}},
    {"The quick brown fox jumps over the lazy dog", 0, {0xe34bbc7bbc071b6c, 0x7a433ca9c49a9347}},
    {"The quick brown fox jumps over the lazy dog", 0x9747b28c, {0x738a7f3bd2633121, 0xf94573727ec016e5}},
};

// The same vectors hold in constant evaluation.
static_assert(ycxx::fingerprint128("abc", 0) == Fp128{0xb4963f3f3fad7867, 0x3ba2744126ca2d52});
static_assert(ycxx::fingerprint128("The quick brown fox jumps over the lazy dog", 0x9747b28c) ==
              Fp128{0x738a7f3bd2633121, 0xf94573727ec016e5});

enum class color : std::uint8_t { red, green };
struct inner {
  std::uint32_t a;
  color c;
};
struct outer {
  inner i;
  std::uint64_t b;
  std::string name;
  std::vector<std::uint16_t> xs;
  std::optional<std::int32_t> o;
};

// A handle whose number is not its identity provides its own fingerprint.
struct handle {
  std::uint32_t index;
};
void hash_into(ycxx::Hasher& h, const handle& x) { h.fp(Fp128{x.index * 7u + 1, 99}); }

}  // namespace

int main() {
  for (const auto& v : vectors) {
    check(ycxx::fingerprint128(v.in, v.seed) == v.out, std::format("vector \"{}\" seed {:#x}", v.in, v.seed));
  }

  // Streaming in every split of every length equals one-shot.
  std::string text;
  for (int k = 0; k < 70; ++k) text.push_back(static_cast<char>('A' + (k * 7) % 53));
  for (std::size_t n = 0; n <= text.size(); ++n) {
    const std::string_view s(text.data(), n);
    const Fp128 whole = ycxx::fingerprint128(s);
    for (std::size_t a = 0; a <= n; a += 3) {
      for (std::size_t b = a; b <= n; b += 5) {
        ycxx::Hasher h;
        h.bytes(s.substr(0, a)).bytes(s.substr(a, b - a)).bytes(s.substr(b));
        check(h.finish() == whole, std::format("split {}/{}/{}", a, b, n));
      }
    }
  }
  check(ycxx::fingerprint128("abc") != ycxx::fingerprint128("abc", 0), "the fixed seed is not zero");

  // Integers are fixed-width little-endian.
  check(ycxx::Hasher().uint(std::uint32_t{0x64636261}).finish() == ycxx::fingerprint128("abcd"),
        "uint is little-endian bytes");

  // Structural fingerprints: equal values agree, every member matters, constant evaluation agrees.
  const outer x{{1, color::green}, 2, "n", {3, 4}, 5};
  outer y = x;
  check(ycxx::fingerprint(x) == ycxx::fingerprint(y), "equal aggregates");
  y.xs.push_back(0);
  check(ycxx::fingerprint(x) != ycxx::fingerprint(y), "a range element");
  y = x;
  y.i.c = color::red;
  check(ycxx::fingerprint(x) != ycxx::fingerprint(y), "a nested enumerator");
  y = x;
  y.o.reset();
  check(ycxx::fingerprint(x) != ycxx::fingerprint(y), "an optional");
  check(ycxx::fingerprint(std::pair{std::string("ab"), std::string("c")}) !=
            ycxx::fingerprint(std::pair{std::string("a"), std::string("bc")}),
        "lengths separate concatenations");
  static_assert(ycxx::fingerprint(inner{1, color::green}) == ycxx::fingerprint(inner{1, color::green}));
  constexpr Fp128 at_compile_time = ycxx::fingerprint(inner{7, color::red});
  check(at_compile_time == ycxx::fingerprint(inner{7, color::red}), "constant evaluation agrees");
  check(ycxx::fingerprint(handle{3}) == ycxx::Hasher().fp(Fp128{22, 99}).finish(), "hash_into is used");
  return ycxx::unit::result("hash");
}
