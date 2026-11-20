extern int missing_weak(int) __attribute__((weak));

int main(void) {
    int (*pointer)(int) = &missing_weak;
    return pointer != 0;
}
