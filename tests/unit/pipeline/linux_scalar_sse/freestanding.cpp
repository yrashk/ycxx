#include <cfloat>

float single_epsilon() { return FLT_EPSILON; }
auto double_epsilon() { return DBL_EPSILON; }

float choose(bool first, float a, float b) { return first ? a : b; }

struct ScalarSource {
  float value;
  float get() const { return value; }
};

template<class T>
T pass(T value) { return value; }

int main() {
  const ScalarSource source{single_epsilon()};
  float single = choose(false, 1.0F, pass(source.get()));
  double wide = pass(double_epsilon());
  const unsigned char* s = static_cast<const unsigned char*>(static_cast<const void*>(&single));
  const unsigned char* w = static_cast<const unsigned char*>(static_cast<const void*>(&wide));
  if (s[0] || s[1] || s[2] || s[3] != 0x34) return 1;
  if (w[0] || w[1] || w[2] || w[3] || w[4] || w[5] || w[6] != 0xb0 || w[7] != 0x3c) return 2;
  return 0;
}
