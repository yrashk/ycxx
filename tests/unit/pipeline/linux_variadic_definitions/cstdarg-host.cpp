#include <cstdarg>

extern "C" int forward_five(int, int, int, int, int, ...);
extern "C" int forward_seven(int, int, int, int, int, int, int, ...);
extern "C" int read_scalar_cursor(std::va_list, const int*);

extern "C" int host_before_start(int value) {
    volatile double fraction = 0.25;
    return value + static_cast<int>(fraction * 8);
}

extern "C" int host_consume_doubles(std::va_list cursor, int count, int expected_first) {
    for (int index = 0; index < count; ++index) {
        if (va_arg(cursor, double) != 10 * count + index + 0.25) {
            return 1;
        }
    }
    int first = va_arg(cursor, int);
    int last = va_arg(cursor, int);
    return first == expected_first && last == 300 + count ? 0 : 2;
}

int host_use_copy(int a, int b, int c, int d, const int* expected_pointer, ...) {
    std::va_list original;
    va_start(original, expected_pointer);
    int first_register_arg = va_arg(original, int);
    std::va_list copy;
    va_copy(copy, original);
    int result = read_scalar_cursor(copy, expected_pointer);
    va_end(copy);

    int number = va_arg(original, int);
    const int* pointer = va_arg(original, const int*);
    long wide = va_arg(original, long);
    va_end(original);
    return result == 0 && first_register_arg == 23 && number == 57 &&
           pointer == expected_pointer && wide == 4294967351L ? 0 : 1;
}

int main() {
    if (forward_five(2, 3, 5, 7, 1, 10.25, 19, 301) != 0) {
        return 1;
    }
    if (forward_five(2, 3, 5, 7, 8, 80.25, 81.25, 82.25, 83.25, 84.25, 85.25,
                     86.25, 87.25, 19, 308) != 0) {
        return 2;
    }
    if (forward_seven(2, 3, 5, 7, 11, 13, 9, 90.25, 91.25, 92.25, 93.25, 94.25,
                      95.25, 96.25, 97.25, 98.25, 43, 309) != 0) {
        return 3;
    }

    const int payload = 71;
    return host_use_copy(2, 3, 5, 7, &payload, 23, 57, &payload, 4294967351L) == 0 ? 0 : 4;
}
