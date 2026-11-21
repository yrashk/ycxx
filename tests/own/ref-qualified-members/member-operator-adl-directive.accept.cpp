namespace payload { struct X { int operator+(int) const &; }; }
namespace foreign { long* operator+(payload::X&&, int); }
namespace payload { using namespace foreign; }
int use() { return payload::X{} + 1; }
