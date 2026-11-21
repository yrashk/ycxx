// STDOUT: assigned members in declaration order
// [class.copy.assign], [dcl.fct.def.default]
extern "C" int puts(const char*);

int calls = 0;

struct Member {
    int value;

    Member& operator=(const Member& source) {
        calls = calls * 10 + source.value;
        value = source.value + 100;
        return *this;
    }
};

struct Record {
    Member first;
    int count;
    Member second;

    Record& operator=(const Record&) noexcept = default;
};

int main() {
    const Record source{{2}, 7, {9}};
    Record destination{{1}, 0, {3}};
    Record& result = destination = source;
    if (&result != &destination)
        return 1;
    if (calls != 29)
        return 2;
    if (destination.first.value != 102 || destination.count != 7)
        return 3;
    if (destination.second.value != 109)
        return 4;

    puts("assigned members in declaration order");
    return 0;
}
