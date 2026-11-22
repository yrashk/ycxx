// Form the substituted operand type before evaluating the trait ([dcl.ptr], #209).
template<class T, bool = __is_reference(T*)> struct Choice {}; Choice<int&> invalid; //! error
