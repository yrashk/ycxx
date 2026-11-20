// [over.ics.list], [over.ics.rank], [dcl.init.list] (#231)
// EXIT: 0
int choose(int value) { return 10 + value; }
int choose(long value) { return 20 + static_cast<int>(value); }

int cross(int first, long second) { return 100 + first + static_cast<int>(second); }
int cross(long first, int second) { return 200 + static_cast<int>(first) + second; }

int next(int& calls) {
    ++calls;
    return 7;
}

int main() {
    if (choose({1}) != 11) return 1;
    if (choose({1L}) != 21) return 2;

    if (cross({3}, {4L}) != 107) return 3;
    if (cross({3L}, {4}) != 207) return 4;

    int calls = 0;
    if (choose({next(calls)}) != 17 || calls != 1) return 5;
    return 0;
}
