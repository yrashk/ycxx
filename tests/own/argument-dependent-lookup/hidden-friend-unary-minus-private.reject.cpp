class Value {
    int operator-() & { return 1; }
    friend int operator-(const Value&) { return 2; }
};
int use(Value& value) { return -value; } //! error
