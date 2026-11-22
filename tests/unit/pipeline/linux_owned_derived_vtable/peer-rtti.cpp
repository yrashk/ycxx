#include "base.hpp"

#include <typeinfo>

const std::type_info& peer_base_type() noexcept { return typeid(B); }

int check_rtti(B& base, D& complete) {
    const std::type_info& dynamic = typeid(base);
    if (dynamic != typeid(D)) return 31;
    if (&dynamic != &typeid(D)) return 32;
    if (dynamic == peer_base_type()) return 33;
    if (dynamic_cast<D*>(&base) != &complete) return 34;
    if (dynamic_cast<void*>(&base) != &complete) return 35;
    return 0;
}
