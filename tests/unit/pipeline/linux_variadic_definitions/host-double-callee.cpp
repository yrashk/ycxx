extern "C" int host_read_double(__builtin_va_list cursor, int expected);

extern "C" int forward_to_host(int expected, ...) {
    __builtin_va_list cursor;
    __builtin_va_start(cursor, expected);
    int result = host_read_double(cursor, expected);
    __builtin_va_end(cursor);
    return result;
}
