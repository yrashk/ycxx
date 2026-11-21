extern "C" int observe_none(int, ...);
extern "C" int observe_named(double, int, ...);
extern "C" int observe_many(int, ...);
extern "C" int observe_full(double, double, double, double, double, double, double, double, int, ...);

int main(int argc, char**) {
    float from_runtime = argc == 1 ? 1.25f : 3.5f;
    if (observe_none(0) != 0) return 1;
    if (observe_named(1.0, 0) != 1) return 2;
    if (observe_named(1.0, 0, 7, 11L) != 1) return 3;
    if (observe_many(0, 1., 2., 3., 4., 5., 6., 7.) != 7) return 4;
    if (observe_many(0, 1., 2., 3., 4., 5., 6., 7., 8.) != 8) return 5;
    if (observe_many(0, 1., 2., 3., 4., 5., 6., 7., 8., 9.) != 8) return 6;
    if (observe_full(1., 2., 3., 4., 5., 6., 7., 8., 0, from_runtime) != 8) return 7;
    return 0;
}
