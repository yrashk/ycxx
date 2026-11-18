// [cpp.concat]/2, /3, [cpp.scope] (the placemarker example): an empty argument next to `##`
// becomes a placemarker; a placemarker pasted with a token gives that token, two placemarkers
// give a placemarker, and remaining placemarkers are removed.
#define t(x,y,z) x ## y ## z
int j[] = { t(1,2,3), t(,4,5), t(6,,7), t(8,9,),
            t(10,,), t(,11,), t(,,12), t(,,) };
#define cat(a, b) a ## b
#define wrap(a, b) [a ## b]
cat(,);
cat(x,);
cat(,y);
wrap(,);
wrap(p,);
#define mid(a) <x ## a ## y>
mid();
mid(1);
