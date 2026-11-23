// #974 [dcl.asm]
#include "symbol.hpp"

struct directive {
  constexpr const char* data() const { return ".hidden probe_value"; }
  constexpr unsigned long size() const { return sizeof(".hidden probe_value") - 1; }
};
consteval directive hide_value() { return {}; }

asm((hide_value()));
extern "C" {
int probe_value = 23;
int neighbor_value = 29;
}
asm((hide_value()));

struct buffered {
  char bytes[sizeof("x?hidden " BUFFERED_SYMBOL "\n.globl unexpected_suffix\n")];
  constexpr const char* data() const { return bytes + 1; }
  constexpr unsigned long size() const { return sizeof(".hidden " BUFFERED_SYMBOL "\n") - 1; }
};
consteval buffered make_buffered() {
  buffered result{"x?hidden " BUFFERED_SYMBOL "\n.globl unexpected_suffix\n"};
  result.bytes[1] = '.';
  return result;
}

[[__gnu__::__weak__]] int weak_target() noexcept { return 31; }
asm((make_buffered()));

struct view {
  const char* text;
  unsigned long length;
  constexpr const char* data() const { return text; }
  constexpr unsigned long size() const { return length; }
};
extern "C" int imported_value() noexcept;
extern "C" int use_import() noexcept { return imported_value(); }
asm((view{".hidden imported_value", sizeof(".hidden imported_value") - 1}));
