// Lexing throughput: `preprocess` over generated code without directives. Not a test: run it from
// an optimized build (YCXX_BUILD_TYPE=Release).
import std;
import ycxx.pp;

int main() {
  std::string text;
  for (int i = 0; text.size() < (8u << 20); ++i) {
    text += std::format("static int f{}(int a, unsigned long b) {{ // comment {}\n", i, i);
    text += "  for (int k = 0; k < 0x10'00; ++k) a += (b >> 3) * 1.5e3f + 'x' + u8\"str\\n\"[1];\n";
    text += "  /* block */ return a <=> b ? a->*b : a::b<c> && !d || e[f] != g;\n}\n";
  }
  auto t0 = std::chrono::steady_clock::now();
  auto p = ycxx::pp::preprocess(text);
  auto t1 = std::chrono::steady_clock::now();
  double s = std::chrono::duration<double>(t1 - t0).count();
  std::println("{} bytes, {} tokens, {} diagnostics: {:.1f} MB/s, {:.0f} ns/token", text.size(), p.region.size(),
               p.diags.size(), double(text.size()) / s / 1e6, s * 1e9 / double(p.region.size()));
}
