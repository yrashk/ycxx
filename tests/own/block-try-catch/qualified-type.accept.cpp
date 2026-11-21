// [except.handle] A handler can name its scalar type through a qualified alias.
namespace N {
  using I = int;
}

int f() {
  try { throw 1; }
  catch (N::I value) { return value; }
}
