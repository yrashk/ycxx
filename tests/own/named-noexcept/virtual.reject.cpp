struct Base {
    virtual void call(char value) noexcept(sizeof(value) == 1);
};
struct Derived : Base {
    void call(char value) noexcept(sizeof(value) != 1) override; //! error
};
