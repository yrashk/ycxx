int trace = 0;

struct Leaf {
    int tag;
    int value;

    Leaf& operator=(const Leaf& other) {
        trace = trace * 10 + tag;
        value = other.value + tag;
        return *this;
    }
};

struct Nested {
    Leaf second;
};

struct Holder {
    Leaf first;
    int scalar;
    Nested nested;
};

int main() {
    const Holder source{{3, 41}, 61, {{4, 71}}};
    Holder destination{{1, 5}, 7, {{2, 9}}};
    Holder& result = (destination = source);

    if (&result != &destination) {
        return 1;
    }
    if (trace != 12) {
        return 2;
    }
    if (destination.first.tag != 1 || destination.first.value != 42 ||
        destination.scalar != 61 || destination.nested.second.tag != 2 ||
        destination.nested.second.value != 73) {
        return 3;
    }

    trace = 0;
    const Holder& alias = destination;
    Holder& self = (destination = alias);
    return &self == &destination && trace == 12 && destination.first.value == 43 &&
                   destination.scalar == 61 && destination.nested.second.value == 75
               ? 0
               : 4;
}
