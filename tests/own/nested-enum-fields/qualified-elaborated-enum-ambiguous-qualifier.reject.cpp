// #761 [basic.lookup.elab], [basic.lookup.qual], [class.member.lookup].
struct LeftGroup { enum Kind { first }; };
struct RightGroup { enum Kind { second }; };

struct Left { using Group = LeftGroup; };
struct Right { using Group = RightGroup; };
struct Both : Left, Right {};

enum Both::Group::Kind* value; //! error
