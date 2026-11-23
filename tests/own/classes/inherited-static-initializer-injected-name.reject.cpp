// [class.pre]: the injected name hides the inherited static value.
struct Base { static constexpr int Derived = 42; };
struct Derived : Base { inline static constexpr int number = Derived; };  //! error
