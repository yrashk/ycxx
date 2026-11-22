// [class.friend], [class.access]: access is granted only to the designated class.
template<class Authority>
class Token {
  friend Authority;
  int scope_ = 17;
};

struct OtherIssuer;
struct Issuer {
  static int accepted(const Token<Issuer>& token) { return token.scope_; }
  static int wrong_issuer(const Token<OtherIssuer>& token) { return token.scope_; }  //! error
  static int nonclass_issuer(const Token<Issuer*>& token) { return token.scope_; }  //! error
};

struct Unrelated {
  static int wrong_class(const Token<Issuer>& token) { return token.scope_; }  //! error
};
