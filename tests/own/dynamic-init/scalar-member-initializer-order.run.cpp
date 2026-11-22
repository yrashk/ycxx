// EXIT: 0
int calls;

int next() {
    return ++calls;
}

struct Record {
    int zero;
    int first = next();
    int derived = first + 1;
};

extern Record one;
int observe_zero_storage() {
    return one.first + calls;
}

int before = observe_zero_storage();
Record one;
int between = one.derived;
Record two{4, 20};
Record three;

int main() {
    if (before != 0 || between != 2 || calls != 2) return 1;
    if (one.zero != 0 || one.first != 1 || one.derived != 2) return 2;
    if (two.zero != 4 || two.first != 20 || two.derived != 21) return 3;
    if (three.zero != 0 || three.first != 2 || three.derived != 3) return 4;
    Record automatic{5, 30};
    if (automatic.derived != 31 || calls != 2) return 5;
    return 0;
}
