// #199, [expr.reinterpret.cast]/2,6,7,8,11: object and function pointer conversions, the
// identity conversions and glvalue conversions keep the address and the object, and evaluate
// the operand once. The byte views assume a little-endian target, which every ycxx target is.
// EXIT: 0
int answer(int x) { return x + 40; }

int* counted(int* p, int& calls) {
  ++calls;
  return p;
}

int& counted_ref(int& r, int& calls) {
  ++calls;
  return r;
}

struct Pair { int first; int second; };
enum Color { red = 3 };

int destroyed = 0;
struct Holder {
  int value = 5;
  ~Holder() { ++destroyed; }
};

int main() {
  int value = 0x01020304;
  int calls = 0;
  char* bytes = reinterpret_cast<char*>(counted(&value, calls));
  if (calls != 1 || static_cast<void*>(bytes) != &value) return 1;
  if (bytes[0] != 4 || bytes[3] != 1) return 2;
  bytes[0] = 5;
  if (value != 0x01020305) return 3;
  // [expr.reinterpret.cast]/7: converting back yields the original pointer value.
  if (reinterpret_cast<int*>(bytes) != &value) return 4;

  const volatile int* qualified = reinterpret_cast<const volatile int*>(bytes);
  if (qualified != &value) return 5;
  const char* const* level = reinterpret_cast<const char* const*>(&bytes);
  if (*level != bytes) return 6;

  int* null = nullptr;
  if (reinterpret_cast<double*>(null) != nullptr) return 7;

  Pair pair{3, 4};
  int* first = reinterpret_cast<int*>(&pair);
  if (first != &pair.first || *first != 3) return 8;
  if (reinterpret_cast<Pair*>(first) != &pair) return 9;

  // /6: a function pointer converted back calls the original function.
  using Opaque = void (*)();
  Opaque opaque = reinterpret_cast<Opaque>(&answer);
  int (*callable)(int) = reinterpret_cast<int (*)(int)>(opaque);
  if (callable(2) != 42) return 10;
  // /8: conditionally supported, and supported on every ycxx target.
  void* code = reinterpret_cast<void*>(&answer);
  if (reinterpret_cast<int (*)(int)>(code)(1) != 41) return 11;

  // /2: a value of integral, enumeration or pointer type converts to its own type.
  if (reinterpret_cast<int>(value) != value || reinterpret_cast<Color>(red) != red) return 12;
  if (reinterpret_cast<int*>(&value) != &value) return 13;

  // /11: a glvalue names the same object through another type.
  calls = 0;
  unsigned& as_unsigned = reinterpret_cast<unsigned&>(counted_ref(value, calls));
  if (calls != 1 || &as_unsigned != reinterpret_cast<unsigned*>(&value)) return 14;
  as_unsigned = 0xffffffffu;
  if (value != -1) return 15;
  const char& low = reinterpret_cast<const char&>(pair.second);
  if (low != 4) return 16;
  unsigned&& xvalue = reinterpret_cast<unsigned&&>(value);
  if (&xvalue != &as_unsigned) return 17;

  // [class.temporary]/6: the cast of a glvalue subobject of a temporary extends its lifetime.
  {
    unsigned&& extended = reinterpret_cast<unsigned&&>(Holder().value);
    if (destroyed != 0 || extended != 5u) return 18;
  }
  if (destroyed != 1) return 19;
  return 0;
}
