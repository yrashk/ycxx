// [except.spec], [conv.fctptr]
template<class T> struct Routine {
    static int ordinary(int);
};

using Nonthrowing = int (*)(int) noexcept;
Nonthrowing cannot_narrow = &Routine<int>::ordinary;  //! error
