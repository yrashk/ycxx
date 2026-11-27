// [expr.type.conv]/1, [dcl.init.list]/3
template<int N> struct Count {};
template<class T> struct Invalid : Count<int{1.5}> {}; //! error
