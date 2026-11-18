// δ s1 (#86), from the step's review: a braced-init-list is either designated or not
// ([dcl.init.general]).
struct D { int x, y; };
D d{1, .y = 2}; //! error
int main() { return d.x; }
