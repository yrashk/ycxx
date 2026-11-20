struct Leaf {
    int value;

    Leaf& operator=(const Leaf& source) {
        value = source.value + 1;
        return *this;
    }
};

struct Whole {
    Leaf member;
    int marker;

    Whole& operator=(int value) {
        marker = value;
        return *this;
    }
};

int main() {
    const Whole source{{17}, 31};
    Whole destination{{5}, 7};

    Whole& copied = (destination = source);
    if (&copied != &destination || destination.member.value != 18 || destination.marker != 31) {
        return 1;
    }

    Whole& from_integer = (destination = 43);
    return &from_integer == &destination && destination.member.value == 18 &&
                   destination.marker == 43
               ? 0
               : 2;
}
