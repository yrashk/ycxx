struct CopyOnly {
    int value;
    constexpr CopyOnly(const CopyOnly&) noexcept = default;
};

void default_construct() {
    CopyOnly value;  // expected-error: user-declared copy constructor leaves no implicit default constructor
}

void try_aggregate_initialization() {
    CopyOnly value{17};  // expected-error: a user-declared constructor prevents aggregate initialization
}
