// [temp.arg.nontype], [temp.constr.atomic], [temp.concept]
enum class Code { ready = 1 };
template<Code Value> concept Ready = Value == Code::ready;

template<class> constexpr Code code = Code::ready;
template<class T> concept Forwarded = Ready<code<T>>;

static_assert(Forwarded<int>);
