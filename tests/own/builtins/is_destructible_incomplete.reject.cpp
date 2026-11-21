struct Incomplete;

bool query() {
    return __is_destructible(Incomplete); //! error
}
