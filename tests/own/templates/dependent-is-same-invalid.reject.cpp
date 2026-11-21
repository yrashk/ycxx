// Substitution into a trait operand may form an invalid type ([dcl.ref], [temp.alias], #209).
template<bool B> struct Bool {};
template<class T> using Compare = Bool<__is_same(T*, int*)>; using Invalid = Compare<int&>; //! error
