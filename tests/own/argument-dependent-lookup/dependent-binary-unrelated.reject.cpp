namespace model {
struct Value {};
}
template<class T> T add(T x, T y) { return x + y; } //! error
namespace unrelated {
model::Value operator+(model::Value, model::Value);
}
model::Value operator+(model::Value, model::Value);
model::Value use() { return add(model::Value{}, model::Value{}); }
