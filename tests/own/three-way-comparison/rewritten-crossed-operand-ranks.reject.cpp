// [over.match.best.general]/2: each orientation has a better conversion for one operand.
struct A {};

int operator<=>(A&, long);
int operator<=>(int, const A&);

bool compare(A& lhs, int rhs) {
    return lhs < rhs;  //! error
}
