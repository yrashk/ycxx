struct Later {
    using Result = decltype(after_alias(static_cast<Later*>(nullptr))); //! error
    friend int after_alias(Later*);
};

struct Deduced {
    friend auto before_alias(Deduced*) { return 17; }
    using Result = decltype(before_alias(static_cast<Deduced*>(nullptr))); //! error
};

struct Evaluated {
    friend constexpr int before_value(Evaluated*) { return 29; }
    static constexpr int value = before_value(static_cast<Evaluated*>(nullptr)); //! error
};
