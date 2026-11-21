struct P {
    short x;
    int y;
};

int check(int input) {
    P point(input, 8);
    return point.x == input && point.y == 8 ? 0 : 1;
}

int main() {
    return check(123);
}
