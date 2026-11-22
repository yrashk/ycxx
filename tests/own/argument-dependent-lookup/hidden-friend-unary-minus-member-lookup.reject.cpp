struct Left { int operator-() const { return 1; } };
struct Right { int operator-() const { return 2; } };
struct Value : Left, Right {
    friend int operator-(Value&) { return 3; }
};
int use(Value& value) { return -value; } //! error
