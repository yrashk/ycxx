extern "C" void fp_values_32(float, float, int*);
extern "C" void fp_values_64(double, double, int*);
extern "C" int fp_branches_32(float, float);
extern "C" int fp_branches_64(double, double);
extern "C" int fp_bool_32(float);
extern "C" int fp_bool_64(double);
extern "C" int fp_context_32(float);
extern "C" int fp_context_64(double);
extern "C" int printf(const char*, ...);

static_assert(sizeof(float) == sizeof(unsigned int));
static_assert(sizeof(double) == sizeof(unsigned long long));

namespace {

enum Operand {
  PositiveZero, NegativeZero, PositiveMin, NegativeMin, One, Two, MinusOne,
  MinusTwo, PositiveInfinity, NegativeInfinity, PositiveNaN, NegativeNaN
};

struct RawInput {
  const char* name;
  volatile unsigned int binary32;
  volatile unsigned long long binary64;
  int truth;
};

// IEEE 754 binary32 and binary64 encodings. Volatile reads keep inputs at runtime.
const RawInput inputs[] = {
    {"+0",        0x00000000U, 0x0000000000000000ULL, 0},
    {"-0",        0x80000000U, 0x8000000000000000ULL, 0},
    {"+min sub",  0x00000001U, 0x0000000000000001ULL, 1},
    {"-min sub",  0x80000001U, 0x8000000000000001ULL, 1},
    {"+1",        0x3f800000U, 0x3ff0000000000000ULL, 1},
    {"+2",        0x40000000U, 0x4000000000000000ULL, 1},
    {"-1",        0xbf800000U, 0xbff0000000000000ULL, 1},
    {"-2",        0xc0000000U, 0xc000000000000000ULL, 1},
    {"+infinity", 0x7f800000U, 0x7ff0000000000000ULL, 1},
    {"-infinity", 0xff800000U, 0xfff0000000000000ULL, 1},
    {"+qNaN",     0x7fc00001U, 0x7ff8000000000001ULL, 1},
    {"-qNaN",     0xffc00002U, 0xfff8000000000002ULL, 1},
};

float load32(const RawInput& input) {
  const unsigned int bits = input.binary32;
  float value;
  __builtin_memcpy(&value, &bits, sizeof(value));
  return value;
}

double load64(const RawInput& input) {
  const unsigned long long bits = input.binary64;
  double value;
  __builtin_memcpy(&value, &bits, sizeof(value));
  return value;
}

constexpr int Lt = 1 << 0;
constexpr int Le = 1 << 1;
constexpr int Gt = 1 << 2;
constexpr int Ge = 1 << 3;
constexpr int Eq = 1 << 4;
constexpr int Ne = 1 << 5;

enum ComparisonResult {
  Less = Lt | Le | Ne,
  Equal = Le | Ge | Eq,
  Greater = Gt | Ge | Ne,
  Unordered = Ne
};

struct Comparison {
  Operand left;
  Operand right;
  ComparisonResult expected;
};

constexpr Comparison comparisons[] = {
    {One, Two, Less},
    {Two, One, Greater},
    {One, One, Equal},
    {MinusTwo, MinusOne, Less},
    {MinusOne, MinusTwo, Greater},
    {PositiveZero, NegativeZero, Equal},
    {NegativeZero, PositiveZero, Equal},
    {PositiveMin, PositiveZero, Greater},
    {PositiveZero, PositiveMin, Less},
    {NegativeMin, NegativeZero, Less},
    {NegativeZero, NegativeMin, Greater},
    {NegativeMin, PositiveMin, Less},
    {NegativeInfinity, MinusTwo, Less},
    {Two, PositiveInfinity, Less},
    {PositiveInfinity, NegativeInfinity, Greater},
    {PositiveInfinity, PositiveInfinity, Equal},
    {NegativeInfinity, NegativeInfinity, Equal},
    {PositiveNaN, One, Unordered},
    {One, PositiveNaN, Unordered},
    {NegativeNaN, NegativeZero, Unordered},
    {NegativeZero, NegativeNaN, Unordered},
    {PositiveNaN, PositiveNaN, Unordered},
    {PositiveNaN, NegativeNaN, Unordered},
};

int check(int width, const char* operation, const char* left, const char* right,
          int actual, int expected) {
  if (actual == expected) return 0;
  printf("f%d %s (%s%s%s) got %d, expected %d\n", width, operation,
         left, right ? ", " : "", right ? right : "", actual, expected);
  return 1;
}

int check_comparisons(const Comparison& comparison) {
  const RawInput& left = inputs[comparison.left];
  const RawInput& right = inputs[comparison.right];
  const float left32 = load32(left);
  const float right32 = load32(right);
  const double left64 = load64(left);
  const double right64 = load64(right);
  int values32[6] = {-1, -1, -1, -1, -1, -1};
  int values64[6] = {-1, -1, -1, -1, -1, -1};
  fp_values_32(left32, right32, values32);
  fp_values_64(left64, right64, values64);

  const char* operators[] = {"<", "<=", ">", ">=", "==", "!="};
  int failures = 0;
  for (int i = 0; i < 6; ++i) {
    const int expected = (comparison.expected >> i) & 1;
    failures += check(32, operators[i], left.name, right.name, values32[i], expected);
    failures += check(64, operators[i], left.name, right.name, values64[i], expected);
  }
  failures += check(32, "comparison branches", left.name, right.name,
                    fp_branches_32(left32, right32), comparison.expected);
  failures += check(64, "comparison branches", left.name, right.name,
                    fp_branches_64(left64, right64), comparison.expected);
  return failures;
}

int check_truth(const RawInput& input) {
  const float value32 = load32(input);
  const double value64 = load64(input);
  const int branch = input.truth ? 17 : 29;
  return check(32, "bool value", input.name, nullptr, fp_bool_32(value32), input.truth)
       + check(64, "bool value", input.name, nullptr, fp_bool_64(value64), input.truth)
       + check(32, "bool branch", input.name, nullptr, fp_context_32(value32), branch)
       + check(64, "bool branch", input.name, nullptr, fp_context_64(value64), branch);
}

}  // namespace

int main() {
  int ordered = 0;
  int unordered = 0;
  for (const Comparison& comparison : comparisons) {
    const int failures = check_comparisons(comparison);
    if (comparison.expected == Unordered) unordered += failures;
    else ordered += failures;
  }
  for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
    const int failures = check_truth(inputs[i]);
    if (i == PositiveNaN || i == NegativeNaN) unordered += failures;
    else ordered += failures;
  }
  if (ordered || unordered) {
    printf("failures ordered=%d unordered=%d\n", ordered, unordered);
    return 1;
  }
  return 0;
}
