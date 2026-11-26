// [expr.cond]/6,7.1, [class.copy.ctor]/1, [dcl.fct.def.delete]/2
struct Token {
  explicit Token(int) noexcept {}
  Token(const Token&) = delete;
};

Token choose_token(bool copy_existing, Token& existing) noexcept {
  return copy_existing ? existing : Token{8}; //! error
}
