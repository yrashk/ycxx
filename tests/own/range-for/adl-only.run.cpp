// [stmt.ranged], [basic.lookup.argdep] (#165, #298)
// EXIT: 0
namespace samples {
struct Range {
    int values[3];
};

int* begin(Range& range) { return range.values; }
int* end(Range& range) { return range.values + 3; }
}

struct Enclosing {
    int begin;
    int end;

    int sum(samples::Range& range) {
        int total = begin + end;
        for (int value : range) {
            total += value;
        }
        return total;
    }
};

int sum_with_local_names(samples::Range& range) {
    int begin = 11;
    int end = 13;
    int total = begin + end;
    for (int value : range) {
        total += value;
    }
    return total;
}

int main() {
    samples::Range range{{2, 3, 5}};
    Enclosing enclosing{4, 7};
    if (enclosing.sum(range) != 21) return 1;
    if (sum_with_local_names(range) != 34) return 2;
    return 0;
}
