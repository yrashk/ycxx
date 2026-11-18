// [cpp.rescan], [cpp.scope], [cpp.null]: macros replaced in declarations, a self-referential
// macro left as a name, an empty macro vanishing, and an #undef making a name ordinary again.
#define EMPTY
#define count count
#define LIMIT 10
#
int count = LIMIT;
EMPTY int EMPTY twice EMPTY = count * 2 EMPTY;
#undef LIMIT
int LIMIT = 3;
int sum() { return count + twice + LIMIT; }
