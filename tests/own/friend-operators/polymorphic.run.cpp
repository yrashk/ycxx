// #1178, [class.friend], [class.virtual]: hidden friends of an abstract class that call virtual
// functions, applied through references and to objects with constructors and destructors.
// EXIT: 0
class Shape {
    virtual int area() const = 0;
    virtual void grow(int) = 0;

public:
    virtual ~Shape() = default;
    friend int operator*(const Shape& s) { return s.area(); }
    friend bool operator!(const Shape& s) { return s.area() == 0; }
    friend Shape& operator++(Shape& s) { s.grow(1); return s; }
    friend Shape& operator*=(Shape& s, int n) { s.grow(n); return s; }
};
struct Square : Shape {
    int side = 0;
    int area() const override { return side * side; }
    void grow(int n) override { side += n; }
};
struct Line : Shape {
    int len = 1;
    int area() const override { return len; }
    void grow(int n) override { len += 10 * n; }
};
int use(Shape& s) { return *(++s *= 2); }

int main() {
    Square q;
    Line l;
    if (!!q || !l) return 1;
    if (use(q) != 9) return 2;
    if (use(l) != 31) return 3;
    return *q == 9 && *l == 31 ? 0 : 4;
}
