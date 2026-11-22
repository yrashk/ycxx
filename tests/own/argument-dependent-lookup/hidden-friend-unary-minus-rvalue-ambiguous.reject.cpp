struct Value {
    int operator-() const { return 1; }
    friend int operator-(const Value&) { return 2; }
};
int use() { return -Value{}; } //! error
