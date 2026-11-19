// [dcl.init.list]/7.3, from #336's review: an unscoped enumeration converted to a floating type in
// list-initialization is not narrowing when it is a constant whose value converts back unchanged.
enum E { e1 = 1 };
enum F : unsigned char { f1 = 3 };
double o{e1};
float p{f1};
int main() { return o == 1 && p == 3 ? 0 : 1; }
