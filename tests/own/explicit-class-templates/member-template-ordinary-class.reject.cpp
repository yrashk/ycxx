// #715: copy-initialization does not consider an explicit constructor template of an ordinary class ([class.conv.ctor], [over.match.copy])
struct S {
    long v;
    template <class U> explicit S(U u) : v(u) {}
};

S direct(1);
S copied = 1; //! error
