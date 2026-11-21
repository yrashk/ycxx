template<class T> T add(T x, T y) { return x + y; }

struct Number { int value; };
Number operator+(Number x, Number y) { return {x.value + y.value}; }
int use_late() { return add(Number{1}, Number{2}).value; }

namespace model {
struct Value { int value; };
}
model::Value operator+(model::Value x, model::Value y) { return {x.value + y.value}; }
template<class T> T add_lexically(T x, T y) { return x + y; }
int use_lexical() { return add_lexically(model::Value{2}, model::Value{3}).value; }
