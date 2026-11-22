struct B {};
struct final;
struct final : B {};
struct D : final {};

namespace objects {
struct Complete {};
struct Complete final;
Complete* address() { return &final; }
}

namespace sealed_name {
struct final final {};
}
