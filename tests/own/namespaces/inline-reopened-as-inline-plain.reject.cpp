// #153: only the first definition of a namespace can make it inline ([namespace.def.general]/3).
namespace b {}
inline namespace b {} //! error
int main() { return 0; }
