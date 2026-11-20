#include <cstdarg>

int read_first(...) {
    std::va_list cursor;
    va_start(cursor);
    int value = va_arg(cursor, int);
    va_end(cursor);
    return value;
}

int main() {
    return read_first(37) == 37 ? 0 : 1;
}
