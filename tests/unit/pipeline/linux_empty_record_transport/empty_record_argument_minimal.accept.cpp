struct Empty {};

int select(Empty) {
    return 17;
}

int main() {
    return select(Empty{}) == 17 ? 0 : 1;
}
