// STDOUT: copied fields and chained destination
// [class.copy.assign], [dcl.fct.def.default]
extern "C" int puts(const char*);

struct Plain {
    int first;
    int second;
    Plain& operator=(const Plain&) = default;
};

struct Marked {
    int value;
    constexpr Marked& operator=(const Marked&) = default;
};

int main() {
    const Plain source{7, -4};
    Plain middle{1, 2};
    Plain destination{3, 5};

    Plain& result = destination = middle = source;
    if (&result != &destination)
        return 1;
    if (destination.first != 7 || destination.second != -4)
        return 2;
    if (middle.first != 7 || middle.second != -4)
        return 3;
    if (source.first != 7 || source.second != -4)
        return 4;

    result.second = 9;
    if (destination.second != 9 || middle.second != -4)
        return 5;

    const Marked marked_source{42};
    Marked marked_destination{0};
    Marked& marked_result = marked_destination = marked_source;
    if (&marked_result != &marked_destination || marked_destination.value != 42)
        return 6;

    puts("copied fields and chained destination");
    return 0;
}
