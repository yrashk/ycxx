// [class.dtor]
struct Private {
    Private() {}
private:
    ~Private() {}
};

void test() {
    (void)Private();  //! error
}
