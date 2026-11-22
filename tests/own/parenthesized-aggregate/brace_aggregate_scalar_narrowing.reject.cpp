struct P {
    int x;
    int y;
};

int main() {
    double fraction = 4.75;
    P point{fraction, 8}; //! error
    return point.x == 4 && point.y == 8 ? 0 : 1;
}
