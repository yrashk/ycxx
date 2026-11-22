struct P {
    int x;
    int y = x + 7;
};

int main() {
    P omitted(4);
    P supplied(9, 3);

    if (omitted.x != 4 || omitted.y != 11)
        return 1;
    if (supplied.x != 9 || supplied.y != 3)
        return 2;
    return 0;
}
