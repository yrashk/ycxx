// ι (#94), from the review: a constinit or constexpr variable default-initialized by a
// constructor that is not constexpr is not constant-initialized ([dcl.constinit], [dcl.constexpr]).
struct S { S() : x(5) {} int x; };
constinit S d; //! error
int main() { return d.x; }
