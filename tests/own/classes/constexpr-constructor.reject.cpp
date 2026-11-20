// [class.ctor.general], [dcl.constexpr]: constructor specifiers must agree.
struct LostConstexpr {
  constexpr LostConstexpr();
};

LostConstexpr::LostConstexpr() {}  //! error

struct AddedConstexpr {
  AddedConstexpr();
};

constexpr AddedConstexpr::AddedConstexpr() {}  //! error

struct StaticConstructor {
  static constexpr StaticConstructor() {}  //! error
};

struct QualifiedConstructor {
  constexpr QualifiedConstructor() const {}  //! error
};
