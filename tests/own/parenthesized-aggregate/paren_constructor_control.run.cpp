struct P {
    int x;
    int y;

    explicit P(int first, int second) : x(first + 10), y(second + 20) {}
};

int main() {
    P point(2, 3);
    return point.x == 12 && point.y == 23 ? 0 : 1;
}
