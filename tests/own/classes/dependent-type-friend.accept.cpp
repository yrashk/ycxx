// [class.friend], [temp.friend]: a specialization grants access to its substituted class.
template<class Authority>
class Token {
  friend Authority;
  int scope_ = 17;
};

struct Issuer {
  static int scope(const Token<Issuer>& token) noexcept { return token.scope_; }
  static int scope(const Token<const Issuer>& token) noexcept { return token.scope_; }
};

union UnionIssuer {
  static int scope(const Token<UnionIssuer>& token) noexcept { return token.scope_; }
  int unused;
};

enum class Mode { ordinary };
Token<int> scalar_token;
Token<Mode> enum_token;
Token<Issuer*> pointer_token;
Token<Issuer&> reference_token;

int main() {
  Token<Issuer> token;
  return Issuer::scope(token) != 17;
}
