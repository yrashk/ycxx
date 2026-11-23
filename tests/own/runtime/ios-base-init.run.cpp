// #842: what `<iostream>` constructs, libycxx's `std::ios_base::Init`, runs in a ycxx-linked
// program. It reaches libycxx's ABI guards and GCC's emulated TLS (`___emutls_get_address`), so
// the default runtime must include libemutls_w.a, and its zero-fill data must start zeroed.
// ycxx cannot declare the nested class yet, so asm labels name its constructor and destructor.
// XFAIL: clang,gcc the symbols are libycxx's; the oracles link their own standard libraries
// STDOUT: init ok
extern "C" void ios_init(void*) __asm__("__ZNSt4__y18ios_base4InitC1Ev");
extern "C" void ios_fini(void*) __asm__("__ZNSt4__y18ios_base4InitD1Ev");
extern "C" int puts(const char*);

unsigned long long object[2];

int main() {
  ios_init(object);
  puts("init ok");
  ios_fini(object);
  return 0;
}
