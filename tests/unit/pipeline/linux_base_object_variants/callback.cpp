struct Callback {
    bool present;
    explicit Callback(int (*function)());
};

Callback::Callback(int (*function)()) : present(function != nullptr) {}
