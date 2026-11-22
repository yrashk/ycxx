struct B {};
struct D final : B {};
struct Rejected : D {}; //! error
