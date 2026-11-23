// #209 [class.access.base], [class.member.lookup], [temp.arg.nontype].
template<int Slots>
struct Budget {};

struct Type { using slots = int; };
struct Value : Type { static constexpr int slots = 3; };
struct PublicConfig : Value {};
struct Config : private Value {};

static_assert(__is_same(Budget<PublicConfig::slots>, Budget<3>));
using Rejected = Budget<Config::slots>; //! error
