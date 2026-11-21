// #102, [expr.const.const], [expr.const.imm]: ordinary runtime uses of a sole immediate function.
consteval int immediate(int x) { return x + 1; }

int (*global_pointer)(int) = immediate; //! error

void take_pointer(int (*function)(int));
void take_reference(int (&function)(int));

struct Receiver {
  Receiver(int (*function)(int)) {}
};

int parenthesized_runtime(int runtime) {
  return (immediate)(runtime); //! error
}

void runtime_uses() {
  int (*local_pointer)(int) = &immediate; //! error
  take_pointer(immediate); //! error
  take_reference(immediate); //! error
  Receiver object(&immediate); //! error
}

// The corresponding initializer with a runtime argument is not constant.
constexpr int call_with_pointer(int (*function)(int), int x) { return function(x); }

int runtime_value(int runtime) {
  int n = call_with_pointer(immediate, runtime); //! error
  return n;
}
