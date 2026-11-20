extern "C" int external_step(int value);
using Step = decltype(&external_step);

extern "C" Step provider_address();
extern "C" int provider_call(int value);

extern "C" {
static int local_step(int value) { return value + 2; }
}

#ifndef EXPECT_STEP
#define EXPECT_STEP 1
#endif

int main() {
    Step pointer = external_step;
    if (pointer != provider_address()) return 1;
    if (pointer(41) != 41 + EXPECT_STEP) return 2;
    if (provider_call(41) != 41 + EXPECT_STEP) return 3;
    Step local_pointer = &local_step;
    if (local_pointer == pointer || local_pointer(41) != 43) return 4;
    return 0;
}
