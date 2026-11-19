// #153, ι (#337), from the review: malformed heads of namespace definitions
// ([namespace.def.general]).
namespace a:: {} //! error
namespace ::q {} //! error
namespace r s {} //! error
namespace t::inline {} //! error
namespace u::v [[deprecated]] {} //! error
namespace [[deprecated]] w::x {} //! error
int main() { return 0; }
