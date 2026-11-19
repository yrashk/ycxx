// #153, ι (#337), from the review: a nested-namespace-definition has no leading `inline`
// ([namespace.def.general]).
inline namespace v1::w {} //! error
int main() { return 0; }
