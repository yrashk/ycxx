// [class.dtor]
struct Private {
    Private() {}
private:
    ~Private() {}
};

void test() {
    Private object;  //! error
}
