// ι (#94), from the review: a variable template's specialization with a dynamic initializer
// ([basic.start.dynamic]: partially-ordered, as an inline variable).
// XFAIL: ycxx variable templates with dynamic initializers (#368)
// EXIT: 0
int g(int n) { return n; }
template <class T> int vt = g(sizeof(T));
int main() { return vt<int> == sizeof(int) ? 0 : 1; }
