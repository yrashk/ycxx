// A static member instantiated for its return type while its class is cannot call a member
// declared later, nor the namespace's function that the member hides ([basic.scope.hiding]). (#1243)
int outer(...);
template <class T> struct Later { static auto a() { return outer(0); } using t = decltype(a()); static char outer(int); }; Later<int>::t later; //! error
