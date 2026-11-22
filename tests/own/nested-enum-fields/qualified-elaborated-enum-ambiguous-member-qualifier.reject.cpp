// #761 [basic.lookup.qual], [basic.lookup.unqual], [class.member.lookup].
struct LeftGroup { enum Kind { first }; };
struct RightGroup { enum Kind { second }; };

struct Left { using Group = LeftGroup; };
struct Right { using Group = RightGroup; };
struct Both : Left, Right {
  enum Group::Kind* value; //! error
};
