extern int (*exception_source)();

extern "C" int pointer_variable() {
  return exception_source();
}

extern "C" int typed_callback(int (*callback)(int), int input) {
  return callback(input);
}

struct Record {
  int value = exception_source();
};

extern "C" int pointer_default() {
  Record record{};
  return record.value;
}

extern "C" int reference_callback(int& (*callback)(int), int input) {
  return callback(input);
}

extern "C" int variadic_callback(int (*callback)(int, ...), int input) {
  return callback(input, 17);
}

extern void note(int) noexcept;

struct Guard {
  int value;
  ~Guard() noexcept { note(value); }
};

extern "C" int guarded_callback(int (*callback)(int) noexcept, int input) {
  Guard guard{input};
  return callback(input);
}
