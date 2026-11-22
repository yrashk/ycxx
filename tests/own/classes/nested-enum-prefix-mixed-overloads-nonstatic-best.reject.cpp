// [over.call.func] rejects the selected nonstatic member because there is no object.
struct Owner {
    int f(int);
    static long long f(long);
    enum Result { width = sizeof(f(0)) }; //! error
};
