struct B { virtual int f() = 0; };
struct D final : B { int f() override { return 0; } };

class R;
class R final {};
class R;

R* pass(R* value) { return value; }
