// [except.spec], [conv.fctptr]
template<class T> struct Routine {
    static int read(int) noexcept;
    static int ordinary(int);
    static int potentially_throwing(int) noexcept(false);
};

using Nonthrowing = int (*)(int) noexcept;
using PotentiallyThrowing = int (*)(int);

Nonthrowing exact = &Routine<int>::read;
PotentiallyThrowing widened = &Routine<int>::read;
PotentiallyThrowing omitted = &Routine<int>::ordinary;
PotentiallyThrowing written_false = &Routine<int>::potentially_throwing;
