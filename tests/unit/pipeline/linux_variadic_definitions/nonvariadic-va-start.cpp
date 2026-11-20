int valid_entry(int named, ...) {
    __builtin_va_list cursor;
    __builtin_va_start(cursor, named);
    __builtin_va_end(cursor);
    return named;
}

int invalid_entry(int named) {
    __builtin_va_list cursor;
    __builtin_va_start(cursor, named);  // expected-error: va_start requires a variadic function
    __builtin_va_end(cursor);
    return named;
}
