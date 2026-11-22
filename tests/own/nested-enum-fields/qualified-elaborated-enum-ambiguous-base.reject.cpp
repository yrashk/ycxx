// #761 [basic.lookup.elab], [class.member.lookup], [dcl.type.elab].
struct Definition { enum Kind { first }; };
enum class OtherKind { second };
struct Alias { using Kind = OtherKind; };
struct Mixed : Definition, Alias {};

enum Mixed::Kind* value; //! error
