extern "C" int check_named_and_promoted(float, double, ...);
extern "C" double check_empty_tail(double, ...);
extern "C" int check_empty_result(double);
extern "C" int check_xmm_count(int, ...);
extern "C" int check_mixed_spills(int, ...);
extern "C" int check_named_eight(double, double, double, double,
                                 double, double, double, double, ...);

extern "C" int run_sse_variadics(float value, float negative_zero, const int* pointer) {
    signed char signed_small = -27;
    unsigned short unsigned_small = 50021;

    if (!check_named_and_promoted(value, 2.25, value, negative_zero, 91.875,
                                  signed_small, unsigned_small, pointer)) {
        return 1;
    }

    double returned = check_empty_tail(11.25);
    if (!check_empty_result(returned)) {
        return 2;
    }

    if (!check_xmm_count(7, 1.25, 2.5, 3.75, 4.125, 5.5, 6.25, 7.875)) {
        return 3;
    }
    if (!check_xmm_count(8, 1.25, 2.5, 3.75, 4.125, 5.5, 6.25, 7.875, 8.5)) {
        return 4;
    }
    if (!check_xmm_count(9, 1.25, 2.5, 3.75, 4.125, 5.5, 6.25, 7.875, 8.5, 9.25)) {
        return 5;
    }

    if (!check_mixed_spills(399,
                           101, 1.25,
                           102, 2.5,
                           103, 3.75,
                           104, 4.125,
                           105, 5.5,
                           106, 6.25,
                           107, 7.875,
                           108, 8.5,
                           109, value,
                           110, 10.75,
                           pointer, negative_zero)) {
        return 6;
    }

    if (!check_named_eight(1.25, 2.5, 3.75, 4.125, 5.5, 6.25, 7.875, 8.5,
                           value, signed_small, negative_zero, unsigned_small, pointer)) {
        return 7;
    }
    return 0;
}
