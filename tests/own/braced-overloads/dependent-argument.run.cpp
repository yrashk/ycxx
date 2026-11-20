// [temp.dep], [over.ics.list] (#231)
// EXIT: 0
int choose(int value) { return 10 + value; }
int choose(long value) { return 20 + static_cast<int>(value); }

template<class T>
int call(T value) {
    return choose({value});
}

int main() {
    if (call(1) != 11) return 1;
    if (call(1L) != 21) return 2;
    return 0;
}
