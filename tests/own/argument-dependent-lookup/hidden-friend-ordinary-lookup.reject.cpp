namespace hidden_calls {
struct Token {
    friend int inspect(const Token&) { return 31; }
};

int qualified(const Token& token) {
    return hidden_calls::inspect(token); //! error
}

int parenthesized(const Token& token) {
    return (inspect)(token); //! error
}
}
