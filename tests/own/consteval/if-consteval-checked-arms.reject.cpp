// [stmt.if]
constexpr int positive() {
    if consteval {
        return missing_positive(); //! error
    } else {
        return 1;
    }
}

constexpr int negative() {
    if !consteval {
        return 2;
    } else {
        return missing_negative(); //! error
    }
}
