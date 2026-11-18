// [cpp.replace.general]: an empty replacement list makes the macro name vanish; the tokens on
// either side stay separate tokens ([cpp.rescan], [lex.pptoken]), so an expansion never pastes
// with its neighbour.
#define EMPTY
#define MINUS -
#define PLUS +
a EMPTY b;
-EMPTY-;
-MINUS;
MINUS-;
MINUS MINUS;
+PLUS;
EMPTY;
x = EMPTY 1 EMPTY;
