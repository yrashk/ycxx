int read_first(...) {
    __builtin_va_list cursor;
    __builtin_va_start(cursor, 0);
    int value = __builtin_va_arg(cursor, int);
    __builtin_va_end(cursor);
    return value;
}

int main() {
    return read_first(37) == 37 ? 0 : 1;
}
