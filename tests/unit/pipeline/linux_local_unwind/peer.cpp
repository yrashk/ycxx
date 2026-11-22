extern "C" void checked(int* count);

namespace {
struct Failure final {
    int payload;
};

constexpr int expected_payload = 663;
bool should_throw = false;
int calls = 0;
}

extern "C" void may_throw() {
    ++calls;
    if (should_throw)
        throw Failure{expected_payload};
}

int main() {
    int count = 0;
    try {
        checked(&count);
    } catch (...) {
        return 1;
    }
    if (count != 1 || calls != 1)
        return 2;

    count = 0;
    should_throw = true;
    try {
        checked(&count);
    } catch (const Failure& failure) {
        if (failure.payload != expected_payload)
            return 3;
        if (count != 1 || calls != 2)
            return 4;
        return 0;
    } catch (...) {
        return 5;
    }
    return 6;
}
