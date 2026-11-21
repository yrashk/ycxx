struct P {
    int x;
    int y;
};

int main() {
    P point(1, 2, 3); //! error
    return point.x;
}
