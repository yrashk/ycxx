const int payload = 73;

int cross_gp_boundary(int a, int b, int c, int d, ...) {
    __builtin_va_list cursor;
    __builtin_va_start(cursor, d);
    int promoted = __builtin_va_arg(cursor, int);

    __builtin_va_list copy;
    __builtin_va_copy(copy, cursor);
    int original_last_register = __builtin_va_arg(cursor, int);
    const int* original_pointer = __builtin_va_arg(cursor, const int*);
    long original_wide = __builtin_va_arg(cursor, long);
    int copied_last_register = __builtin_va_arg(copy, int);
    const int* copied_pointer = __builtin_va_arg(copy, const int*);
    long copied_wide = __builtin_va_arg(copy, long);
    int original_tail = __builtin_va_arg(cursor, int);
    int copied_tail = __builtin_va_arg(copy, int);
    __builtin_va_end(copy);
    __builtin_va_end(cursor);

    return promoted == -9 && original_last_register == 17 && copied_last_register == 17 &&
           original_pointer == &payload && copied_pointer == &payload &&
           original_wide == 4294967351L && copied_wide == 4294967351L &&
           original_tail == 29 && copied_tail == 29;
}

int all_gp_named(int a, int b, int c, int d, int e, int f, ...) {
    __builtin_va_list cursor;
    __builtin_va_start(cursor, f);
    int first_stack_arg = __builtin_va_arg(cursor, int);
    __builtin_va_end(cursor);
    return first_stack_arg;
}

int main() {
    if (!cross_gp_boundary(1, 2, 3, 4, static_cast<short>(-9), 17, &payload, 4294967351L, 29)) {
        return 1;
    }
    return all_gp_named(1, 2, 3, 4, 5, 6, 47) == 47 ? 0 : 2;
}
