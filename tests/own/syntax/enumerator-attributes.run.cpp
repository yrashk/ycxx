// #212, from the review: enumerators may carry attributes ([dcl.enum]/1); the comma check
// does not take them for a missing comma.
// EXIT: 0
enum E { A [[deprecated]], B [[maybe_unused]] = 3 };
int main() { return B - 3; }
