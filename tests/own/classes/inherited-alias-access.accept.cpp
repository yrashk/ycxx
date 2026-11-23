namespace protected_names {
// [class.access.general], [basic.lookup.unqual]: a derived member can name a protected alias.
struct Base {
protected:
  using T = char;
  typedef short S;
};
struct Derived : Base {
  using Published = T;
  int member() { return sizeof(T) + sizeof(S); }
  int shadow() { using T = long; return sizeof(T); }
};
static_assert(sizeof(Derived::Published) == 1);
}

namespace own_names {
// [class.member.lookup]: a preceding own declaration hides a private inherited alias.
struct Base { private: using T = char; };
struct WithAlias : Base {
  using T = short;
  using Published = T;
};
struct WithEnum : Base {
  enum class T : short {};
  using Published = T;
};
static_assert(sizeof(WithAlias::Published) == sizeof(short));
static_assert(sizeof(WithEnum::Published) == sizeof(short));
}

namespace one_accessible_path {
// [class.member.lookup], [class.access.base]: a usable base path suffices.
struct Origin { using T = char; };
struct Hidden : private Origin {};
struct Exposed : Origin {};
struct Combined : Hidden, Exposed {
  using Published = T;
  int member() { return sizeof(T); }
};
static_assert(sizeof(Combined::Published) == 1);
}

namespace declared_access {
// [class.access.general]: access checks the named alias rather than its designated type.
struct Friend;
struct Base {
private:
  enum class Implementation : unsigned char {};
  using T = char;
  friend struct Friend;
public:
  using Public = Implementation;
};
struct Friend : Base {
  using Alias = T;
  int member() { return sizeof(T); }
};
struct Derived : Base {
  using Published = Public;
  int member() { return sizeof(Public); }
};
static_assert(sizeof(Friend::Alias) == 1);
static_assert(sizeof(Derived::Published) == 1);
}
