constexpr void operator delete[](void*, void*) noexcept {}

constexpr void operator delete[](void*, void*) noexcept(false); //! error
