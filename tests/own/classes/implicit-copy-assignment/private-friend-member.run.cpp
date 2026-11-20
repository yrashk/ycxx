struct Holder;

struct Leaf {
    int value;

private:
    friend struct Holder;

    Leaf& operator=(const Leaf& other) {
        value = other.value + 1;
        return *this;
    }
};

struct Holder {
    Leaf member;
};

int main() {
    Holder destination{{5}};
    const Holder source{{37}};
    Holder& result = (destination = source);
    return &result == &destination && destination.member.value == 38 ? 0 : 1;
}
