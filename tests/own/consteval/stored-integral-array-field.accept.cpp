struct Entry {
    unsigned long long limbs[3];
    int tag;
};

inline constexpr Entry entry{{5ULL, 0xfedcba9876543210ULL, 17ULL}, 23};

consteval unsigned long long selected_limb() {
    const Entry& selected = entry;
    return selected.limbs[1];
}

static_assert(selected_limb() == 0xfedcba9876543210ULL);
static_assert(entry.tag == 23);
