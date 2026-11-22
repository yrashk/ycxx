struct Value {
    friend int operator-(Value&) = delete;
    friend int operator-(const Value&) { return 1; }
};
int use(Value& value) { return -value; } //! error
