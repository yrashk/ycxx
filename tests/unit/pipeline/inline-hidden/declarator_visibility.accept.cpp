int one [[gnu::visibility("hidden")]](int argument), two(int sibling);

int call(int n) {
    return one(n) + two(n);
}
