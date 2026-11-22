// A non-bool atom reached during evaluation is ill-formed even in a disjunction.
template<class T> concept Wrong = T{}; template<class T> concept ReachesWrong = Wrong<T> || true; int main() { ReachesWrong auto value = 1; } //! error
