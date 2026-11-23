// #209 [temp.arg.general], [temp.names], [temp.variadic].
namespace Check {
template<class T, class... Args>
concept Fits = sizeof(T) > 0;
}

template<bool Result>
struct Bool {};

template<class... Args>
struct Record : Bool<Check::Fits<Args...>> {}; //! error
