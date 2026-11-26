import std;
import ycxx.bench;

using Word = unsigned long long;
extern "C" Word ENTRY(Word, Word, Word);

int main() {
  using namespace ycxx::bench;
  auto config = configuration_for(Band::standard);
  config.process_telemetry = true;
  auto result = measure_batched([](Iterations iterations) {
    Word x = 1;
    Word salt = 0;
    for (Iterations i = 0; i != iterations; ++i) {
      x = x * 6364136223846793005ULL + 1442695040888963407ULL;
      salt = ENTRY(x, (x >> 41) + 1, salt);
    }
    return salt;
  }, config);
  write_ndjson(std::cout, LABEL, result);
}
