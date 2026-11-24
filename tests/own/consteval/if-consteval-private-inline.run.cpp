// [stmt.if], [dcl.constexpr]
class Choice {
    static constexpr int compile_time(int value) { return value + 20; }

public:
    static constexpr int positive(int value) {
        if consteval {
            return compile_time(value);
        } else {
            return value + 1;
        }
    }

    static constexpr int negative(int value) {
        if !consteval {
            return value - 3;
        } else {
            return compile_time(value) + 2;
        }
    }
};

static_assert(Choice::positive(4) == 24);
static_assert(Choice::negative(4) == 26);

int main() {
    volatile int input = 4;
    if (Choice::positive(input) != 5)
        return 1;
    if (Choice::negative(input) != 1)
        return 2;
    return 0;
}
