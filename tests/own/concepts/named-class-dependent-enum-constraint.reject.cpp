// [temp.arg.nontype], [temp.constr.decl], [temp.inst]
enum class Choice { preferred, alternate };
template<class T, T Value> requires (Value == T::preferred)
struct Selected;
Selected<Choice, Choice::alternate>* selected; //! error
