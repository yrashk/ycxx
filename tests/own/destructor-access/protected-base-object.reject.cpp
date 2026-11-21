// [class.protected]
struct Base { protected: ~Base(); };
struct Derived : Base { static void check(); };

void Derived::check() {
    Base object;  //! error
    Base{};       //! error
}
