namespace audit {
int check(int value);

struct Token {
    friend int audit::check(int value) { return value; } //! error
};
}
