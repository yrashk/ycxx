// [class.compare.default]/2: a class with a reference member defines its defaulted
// equality operator as deleted.
struct O {
    int& r;
    friend bool operator==(const O&, const O&) = default;
};

bool f(const O& a, const O& b) {
    return a == b;  //! error
}
