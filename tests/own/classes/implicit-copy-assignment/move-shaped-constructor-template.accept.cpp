struct S {
    int value;
    template<class T> S(S&&);
};

void copy(S& destination, const S& source) {
    destination = source;
}
