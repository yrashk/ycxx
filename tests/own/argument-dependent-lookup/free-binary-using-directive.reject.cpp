namespace A { struct Value {}; }
namespace Other { int operator+(A::Value, int); }
namespace A { using namespace Other; }

int use(A::Value v) { return v + 1; } //! error
