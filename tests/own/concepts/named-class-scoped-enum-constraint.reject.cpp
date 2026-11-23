// [temp.arg.nontype], [temp.constr.decl], [temp.inst]
enum class Choice { preferred, alternate };
template<Choice Value> requires (Value == Choice::preferred)
struct Selected;
Selected<Choice::alternate>* selected; //! error
