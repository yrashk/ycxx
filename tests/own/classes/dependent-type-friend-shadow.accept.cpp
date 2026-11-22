// [temp.local]: a type in a nondependent base hides a template parameter.
struct Right;
struct Ancestor { using Authority = Right; };
struct Immediate : protected Ancestor {};
template<class Authority> class Token : public Immediate {
  friend Authority;
  int key = 1;
};
struct Right {
  static int see(const Token<int>& t) { return t.key; }
};

// [temp.dep]: a dependent base stays outside unqualified lookup after substitution.
struct ParameterIssuer;
struct BaseIssuer;

template<class T>
struct Base;

template<>
struct Base<ParameterIssuer> {
  using Authority = BaseIssuer;
};

template<class Authority>
class Vault : public Base<Authority> {
  friend Authority;
  int value = 3;
};

struct ParameterIssuer {
  static int read(const Vault<ParameterIssuer>& vault) { return vault.value; }
};
