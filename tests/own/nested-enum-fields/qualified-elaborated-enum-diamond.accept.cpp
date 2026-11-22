// #761 [basic.lookup.elab], [class.member.lookup], [dcl.type.elab].
struct Origin { enum class Kind { first }; };
struct Left : Origin {};
struct Right : Origin {};
struct Diamond : Left, Right {};

enum Diamond::Kind value = Origin::Kind::first;
static_assert(__is_same(decltype(value), Origin::Kind));
