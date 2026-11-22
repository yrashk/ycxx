namespace repeated {
struct Token {};

struct First {
    friend int inspect(const Token&) { return 1; }
};

struct Second {
    friend int inspect(const Token&) { return 2; } //! error
};
}
