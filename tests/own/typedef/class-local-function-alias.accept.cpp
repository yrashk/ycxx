// [dcl.name], [dcl.fct], [dcl.typedef] (#216)
struct S {
  using F = S(S);
  using Result = S;
  using G = S::Result(S);
};

extern S::F *operation;
extern S::G *operation;
extern S (*operation)(S);
