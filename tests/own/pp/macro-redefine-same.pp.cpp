// [cpp.replace.general]/2: an object-like macro may be redefined with an identical replacement
// list; whitespace separations count as identical whatever their length or form (comments
// are whitespace, [lex.phases]/1.3).
#define SUM 1 + 2
#define SUM 1   +    2
#define SUM 1 /* one */ + /* two */ 2
#define SUM   1 + 2   
#define NOTHING
#define NOTHING
SUM;
NOTHING;
