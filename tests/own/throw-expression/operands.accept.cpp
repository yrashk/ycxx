// [expr.throw], [except.throw] Scalars and pointers form exception objects.
enum class Signal { stop };
struct Complete { int value; };

void raise(int& number, const int fixed, const volatile void* erased, Complete* object) {
  throw number = fixed;
  throw Signal::stop;
  throw 0.5;
  throw nullptr;
  throw erased;
  throw object;
}
