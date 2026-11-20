// Adjusted parameter types correspond across aliases and cv ([dcl.fct], #455).
namespace N {
using Element = unsigned long;
using Bias = int;
extern "C" int adjusted(Element samples[2], const Bias bias);
int adjusted(unsigned long* samples, int bias) {
  return static_cast<int>(samples[1]) + bias;
}
}
int main() {
  unsigned long values[2] = {19, 23};
  return N::adjusted(values, 19) == 42 ? 0 : 1;
}
