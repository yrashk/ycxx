struct Base {
    virtual void strict(char value) noexcept(sizeof(value) == 1);
    virtual void loose(char value) noexcept(sizeof(value) != 1);
};
struct Derived : Base {
    void strict(char renamed) noexcept override;
    void loose(char renamed) noexcept override;
};
