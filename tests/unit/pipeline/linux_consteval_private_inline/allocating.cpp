// [stmt.if], [dcl.constexpr], [expr.const.core]
using Size = decltype(sizeof(0));

class Buffer {
    static constexpr int clone(Size size) {
        char* memory = new char[size + 1];
        delete[] memory;
        return 42;
    }

public:
    static constexpr int positive(Size size) {
        if consteval {
            return clone(size);
        } else {
            return 41;
        }
    }

    static constexpr int negative(Size size) {
        if !consteval {
            return 17;
        } else {
            return clone(size);
        }
    }
};

int positive_runtime(Size size) { return Buffer::positive(size); }
int negative_runtime(Size size) { return Buffer::negative(size); }
