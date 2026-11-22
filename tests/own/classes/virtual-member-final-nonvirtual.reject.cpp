// [class.virtual]: only a virtual member function can be final.
struct Ordinary {
    int score(int) const final;  //! error
};
