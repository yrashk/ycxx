#include <cstdarg>

int read_first(int named, ...) {
    int calls = 0;
    std::va_list cursor;
    va_start(cursor, named, ++calls, unavailable_name);
    int value = va_arg(cursor, int);
    va_end(cursor);
    return calls == 0 ? value : -1;
}

int main() {
    return read_first(7, 31) == 31 ? 0 : 1;
}
