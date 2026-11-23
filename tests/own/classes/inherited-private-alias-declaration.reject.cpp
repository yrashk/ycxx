// [class.access.general], [dcl.typedef]: an alias declaration checks its referenced alias.
struct Base {
private:
  using T = char;
};
struct Derived : Base {
  using Alias = T; //! error
};
