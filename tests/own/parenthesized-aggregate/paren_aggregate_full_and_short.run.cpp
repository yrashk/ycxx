struct P {
    int x;
    int y;
};

int main() {
    P full(6, 8);
    P short_init(9);

    if (full.x != 6 || full.y != 8)
        return 1;
    if (short_init.x != 9 || short_init.y != 0)
        return 2;
    return 0;
}
