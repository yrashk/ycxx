typedef unsigned long Wide __attribute__((mode(TI)));

enum class Value : Wide { zero };

struct Base {
    bool nonzero;
    explicit Base(Value);
};

Base::Base(Value value) : nonzero(value != Value::zero) {}
