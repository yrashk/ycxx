// #1227: satisfying the first operand of a disjunction skips substitution
// into the second constraint's parameter mapping ([temp.constr.op]).
template<class T> struct Check {
    static_assert(sizeof(T) == 1, "second constraint instantiated");
    using Type = T;
};

template<class T> concept Complete = sizeof(T) > 0;
template<class T>
    requires (true || Complete<typename Check<T>::Type>)
using Identity = T;

Identity<long>* accepted;
