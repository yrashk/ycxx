// #157 [class.mem.general], [dcl.fct.def.default], [expr.const]: a member function body is a
// complete-class context, so a constant evaluation after the class can call, from a body written
// earlier in the class, a defaulted constructor or another member declared after that body. This is
// the shape of the library's source_location: a consteval `current()` above `source_location() = default`.
// EXIT: 0
struct Location {
private:
  struct Impl {
    const char* file;
    unsigned line;
  };
  const Impl* impl = nullptr;

public:
  static consteval Location current(const void* p = nullptr) noexcept {
    Location s;
    s.impl = static_cast<const Impl*>(p);
    return s;
  }
  constexpr Location() noexcept = default;
  constexpr unsigned line() const noexcept { return impl ? impl->line : 0u; }
};
constexpr Location recorded = Location::current();
static_assert(recorded.line() == 0);

struct Later {
  int v = 4;
  static constexpr Later make() { return Later(); }
  static constexpr int twice() { return helper() * 2; }
  constexpr Later() = default;
  static constexpr int helper() { return 21; }
};
static_assert(Later::make().v == 4);
static_assert(Later::twice() == 42);

// An enclosing class's members are declared in a nested class's member functions too.
struct Outer {
  struct Inner {
    static constexpr int get() { return Outer::value(); }
  };
  static constexpr int value() { return 7; }
};
static_assert(Outer::Inner::get() == 7);

template <class T> struct Box {
  T* p = nullptr;
  static constexpr Box make() { Box b; return b; }
  constexpr Box() = default;
};
static_assert(Box<int>::make().p == nullptr);

Location capture() { return Location::current(); }

int main() {
  if (capture().line() != 0) return 1;
  if (Later::make().v != 4 || Later::twice() != 42) return 2;
  return Box<char>::make().p != nullptr ? 3 : 0;
}
