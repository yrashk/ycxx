// [over.match.viable], [over.ics.list] (#231)
// EXIT: 0
int choose_by_arity(int value) { return 10 + value; }
int choose_by_arity(const int&, int);

struct Constructed {
    Constructed(int);
};

int choose_by_sibling(int value, const int* sibling) {
    return 20 + value + *sibling;
}

int choose_by_sibling(Constructed, int*);

int main() {
    const int sibling = 7;
    if (choose_by_arity({3}) != 13) return 1;
    if (choose_by_sibling({5}, &sibling) != 32) return 2;
    return 0;
}
