int fixed_bias(int value) {
    return value + 3;
}

int sample(int value) {
    if (value == 0)
        return 8;
    return fixed_bias(value) * 2;
}

int main() {
    if (sample(0) != 8)
        return 1;
    return sample(18);
}
