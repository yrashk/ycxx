int trace = 0;

struct Leaf {
    int value;

    Leaf& operator=(const Leaf& other) {
        trace = trace * 10 + 3;
        value = other.value;
        return *this;
    }
};

struct Holder {
    Leaf member;
};

Holder& select_left(Holder& object) {
    trace = trace * 10 + 2;
    return object;
}

const Holder& select_right(const Holder& object) {
    trace = trace * 10 + 1;
    return object;
}

int main() {
    Holder destination{{7}};
    const Holder source{{37}};
    Holder& result = (select_left(destination) = select_right(source));
    return trace == 123 && &result == &destination && destination.member.value == 37 ? 0 : 1;
}
