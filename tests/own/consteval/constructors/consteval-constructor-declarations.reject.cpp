// #102, [dcl.constexpr]/1–2: every declaration of a consteval function has the specifier, and a
// destructor cannot be consteval.
struct Declared {
  consteval Declared(int);
  Declared(long);
};
Declared::Declared(int) {} //! error
consteval Declared::Declared(long) {} //! error

struct Destroyed {
  consteval ~Destroyed() {} //! error
};
