// [over.ics.list], [over.ics.rank], [dcl.init.list] (#231)
// EXIT: 0
struct P {
    int first;
    int second;
};

int choose(P value) { return 10 * value.first + value.second; }
int choose(long value) { return 100 + static_cast<int>(value); }

int with_ellipsis(P value) { return 300 + 10 * value.first + value.second; }
int with_ellipsis(...) { return 400; }

int main() {
    if (choose({1, 2}) != 12) return 1;
    if (choose({1}) != 101) return 2;
    if (choose({}) != 100) return 3;
    if (with_ellipsis({3, 4}) != 334) return 4;
    if (with_ellipsis(5) != 400) return 5;
    return 0;
}
