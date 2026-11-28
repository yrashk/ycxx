// __is_convertible through a user-defined conversion (#1680): a converting constructor
// ([class.conv.ctor]) or a conversion function ([class.conv.fct]) between types that are not
// reference-related ([conv.general]/3, [over.best.ics], [over.match.copy], [over.match.conv]).

// A converting constructor, after an lvalue-to-rvalue or array-to-pointer conversion.
struct V {
  V(const char*);
};
static_assert(__is_convertible(const char* const&, V));
static_assert(__is_convertible(const char (&)[4], V));
static_assert(__is_convertible(const char*, V));
static_assert(__is_convertible(char*, V));
static_assert(!__is_convertible(int, V));
static_assert(!__is_convertible(const void*, V));
// A glvalue of type std::nullptr_t converts to a pointer after the lvalue-to-rvalue
// conversion ([conv.ptr]/1).
static_assert(__is_convertible(decltype(nullptr)&, V));
static_assert(__is_convertible(const decltype(nullptr)&, V));
// A reference to const or an rvalue reference binds to the converted temporary; a reference
// to non-const cannot ([dcl.init.ref]/5.4).
static_assert(__is_convertible(const char*, const V&));
static_assert(__is_convertible(const char*, V&&));
static_assert(!__is_convertible(const char*, V&));

// A converting constructor of a class template specialization, with a deleted overload.
template <class C>
struct View {
  View(const C*);
  View(decltype(nullptr)) = delete;
  template <class I, class E>
  View(I, E);
};
static_assert(__is_convertible(const char*, View<char>));
static_assert(__is_convertible(const char (&)[64], View<char>));
static_assert(!__is_convertible(decltype(nullptr), View<char>));
static_assert(!__is_convertible(const int*, View<char>));

// A conversion function, then a standard conversion.
struct S {
  operator const char*() const;
};
static_assert(__is_convertible(const S&, const char*));
static_assert(__is_convertible(S, const char*));
static_assert(__is_convertible(S&, const void*));
static_assert(__is_convertible(S, bool));
static_assert(!__is_convertible(S, char*));
static_assert(!__is_convertible(S, int));
// A conversion function to a class.
struct Viewed {
  operator View<char>() const;
};
static_assert(__is_convertible(const Viewed&, View<char>));
static_assert(!__is_convertible(const Viewed&, const char*));

// The implicit object parameter decides viability.
struct Moved {
  operator int() &&;
};
static_assert(__is_convertible(Moved, int));
static_assert(!__is_convertible(Moved&, int));
struct Mutable {
  operator int();
};
static_assert(__is_convertible(Mutable&, int));
static_assert(!__is_convertible(const Mutable&, int));

// The second standard conversion ranks the candidates ([over.match.best.general]/2.2).
struct Two {
  operator int() const;
  operator double() const;
};
static_assert(__is_convertible(Two, int));
static_assert(__is_convertible(Two, double));
static_assert(!__is_convertible(Two, long));  // ambiguous: two conversions

// The trait is false when the selected conversion is explicit, ambiguous, deleted or
// inaccessible.
struct Explicit {
  explicit Explicit(int);
};
static_assert(!__is_convertible(int, Explicit));
struct ExplicitFunction {
  explicit operator int() const;
};
static_assert(!__is_convertible(ExplicitFunction, int));
struct Ambiguous {
  Ambiguous(int);
  Ambiguous(long);
};
static_assert(!__is_convertible(unsigned, Ambiguous));
static_assert(__is_convertible(int, Ambiguous));
struct Deleted {
  Deleted(int) = delete;
  Deleted(double);
};
static_assert(!__is_convertible(int, Deleted));
static_assert(__is_convertible(double, Deleted));
class Private {
  Private(int);
  operator int() const;
};
static_assert(!__is_convertible(int, Private));
static_assert(!__is_convertible(Private, int));

// A captureless closure converts to a pointer to function by its conversion function
// ([expr.prim.lambda.closure]/9), also from a generic lambda's call operator template.
auto plain = [](int x) { return x; };
static_assert(__is_convertible(decltype(plain), int (*)(int)));
static_assert(__is_convertible(decltype(plain)&, int (*)(int)));
auto generic = [](auto x) { return x; };
static_assert(__is_convertible(decltype(generic), int (*)(int)));
// A conversion function to an enumeration, then an integral promotion.
enum Small { small };
struct ToEnum {
  operator Small() const;
};
static_assert(__is_convertible(ToEnum, int));

// A scalar is constructible from an lvalue of a type that converts to it.
static_assert(__is_constructible(const char*, const char*&));
static_assert(__is_constructible(const char*, char* const&));
static_assert(__is_constructible(const void*, const char*&));
static_assert(!__is_constructible(char*, const char*&));
static_assert(!__is_constructible(int*, long*));
static_assert(!__is_constructible(const char*, const char*, int));
// Direct-initialization converts a null pointer value to bool; copy-initialization does not.
static_assert(__is_constructible(bool, decltype(nullptr)));
static_assert(!__is_convertible(decltype(nullptr), bool));
static_assert(__is_constructible(bool, int*));
static_assert(!__is_constructible(int, int*));

int main() {}
