// #153, ι (#337), from the review: every component of a nested namespace definition names
// nothing else in its scope ([basic.scope.scope]/4).
namespace a { int b = 5; }
namespace a::b { int f() { return 1; } } //! error
namespace c { struct d {}; }
namespace c::d::e {} //! error
int main() { return a::b; }
