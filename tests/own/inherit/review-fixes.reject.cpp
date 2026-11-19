// #308, from the review: a const member of a base deletes the derived class's implicit copy
// assignment too ([class.copy.assign]/7). (One line: GCC reports the implicit definitions too.)
struct K { const int k = 1; }; struct L : K {}; void f(L& a, const L& b) { a = b; }  //! error
int main() { return 0; }
