// [dcl.mptr] (#238)
template<class M, class C>
concept R = requires(M C::* p) { p; };
