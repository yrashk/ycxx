// [temp.arg.nontype], [temp.constr.atomic], [temp.concept]
enum class Code { ready = 1 };
enum class Other { ready = 1 };

template<Code Value> concept Ready = Value == Code::ready;
template<class T, T Value> concept Forwarded = Ready<Value>;

static_assert(Forwarded<Code, Code::ready>);
static_assert(!Forwarded<Other, Other::ready>);
static_assert(!Forwarded<int, 1>);
static_assert(!Forwarded<decltype(nullptr), nullptr>);
