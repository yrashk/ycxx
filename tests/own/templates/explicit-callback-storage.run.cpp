// PEER: explicit-callback-storage-peer.cpp

template<class Key>
struct ControlAdjustment {
    static auto apply(int input) { return 5L * input - 2L; }
};

using ExplicitCallback = long (*)(int);

extern "C" {
    ExplicitCallback explicit_callback = &ControlAdjustment<int>::apply;
    int verify_explicit_callback();
}

int main() { return verify_explicit_callback(); }
