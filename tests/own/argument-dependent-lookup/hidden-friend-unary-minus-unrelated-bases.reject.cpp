struct Left { int operator-() { return 1; } };
struct Right {};
struct Value : Left, Right {
    friend int operator-(const Right&) { return 2; }
};
int use(Value& value) { return -value; } //! error

struct Intermediate : Right {};
struct Deeper : Left, Intermediate {
    friend int operator-(const Right&) { return 3; }
};
int use(Deeper& value) { return -value; } //! error
