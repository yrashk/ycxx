// Naming a base that is a private base of an intermediate class through its inherited
// injected-class-name is rejected in a member using-declaration ([class.access.base]/5,
// [class.member.lookup], [namespace.udecl]/14). (#1150)
// XFAIL: ycxx access to an inherited injected-class-name is not checked (#1427)
class A {
public:
  int pub();
};
class Mid : A {};
struct F : Mid {
  using A::pub;     //! error
};
int main() { return 0; }
