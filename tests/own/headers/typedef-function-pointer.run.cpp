// #133: `T (*p)(args);` with T a typedef-name declares p, as the SDK's FILE declares _seek: a
// ptr-operator begins a nested declarator ([stmt.ambig], [dcl.ambig.res]).
// EXIT: 0
typedef long pos;
struct F {
  pos (*seek)(void*, pos, int);
};
pos s(void*, pos p, int) { return p + 1; }
extern pos (*global)(void*, pos, int);  // declared only: address constants are #101
int main() {
  F f{s};
  pos (*local)(void*, pos, int) = s;
  return (f.seek == s ? 0 : 1) + (local == s ? 0 : 1);
}
