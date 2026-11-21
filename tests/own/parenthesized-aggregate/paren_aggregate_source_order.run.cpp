struct P {
    int x;
    int y;
};

int append_digit(int* trace, int digit) {
    *trace = *trace * 10 + digit;
    return *trace;
}

int main() {
    int trace = 0;
    P point(append_digit(&trace, 1), append_digit(&trace, 2));

    if (point.x != 1 || point.y != 12)
        return 1;
    return trace == 12 ? 0 : 2;
}
