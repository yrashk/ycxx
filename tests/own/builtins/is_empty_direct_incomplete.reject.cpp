struct Incomplete;

bool query() {
    return __is_empty(Incomplete); //! error
}
