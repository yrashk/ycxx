// [expr.cond]/6,7.1, [class.copy.ctor]/1, [class.access.general]/1
class Token {
public:
  explicit Token(int) noexcept {}

private:
  Token(const Token&) noexcept;
};

Token choose_token(Token& existing) noexcept {
  return false ? existing : Token{8}; //! error
}
