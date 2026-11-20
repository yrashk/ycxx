#include <cstdarg>

extern "C" int host_before_start(int value);
extern "C" int host_consume_doubles(std::va_list cursor, int count, int expected_first);

extern "C" int forward_five(int a, int b, int c, int d, int count, ...) {
    int expected_first = host_before_start(a + b + c + d);
    std::va_list cursor;
    va_start(cursor, count);
    int result = host_consume_doubles(cursor, count, expected_first);
    va_end(cursor);
    return result;
}

extern "C" int forward_seven(int a, int b, int c, int d, int e, int f, int count, ...) {
    int expected_first = host_before_start(a + b + c + d + e + f);
    std::va_list cursor;
    va_start(cursor, count);
    int result = host_consume_doubles(cursor, count, expected_first);
    va_end(cursor);
    return result;
}

extern "C" int read_scalar_cursor(std::va_list cursor, const int* expected_pointer) {
    int number = va_arg(cursor, int);
    const int* pointer = va_arg(cursor, const int*);
    long wide = va_arg(cursor, long);
    return number == 57 && pointer == expected_pointer && wide == 4294967351L ? 0 : 1;
}
