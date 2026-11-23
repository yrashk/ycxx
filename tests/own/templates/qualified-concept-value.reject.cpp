// #209 [temp.names], [temp.concept].
namespace Check {
template<class T, class... A>
concept Fits = sizeof(T) > 0;
}

template<bool Result> struct Bool {};
Bool<Check::Fits<>> wrong_arity; //! error
