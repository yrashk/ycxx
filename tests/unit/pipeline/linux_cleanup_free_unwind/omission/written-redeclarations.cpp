extern "C" int peer_raise(int);

extern "C" int prior(int) noexcept(false);
extern "C" int prior(int value) { return peer_raise(value) + 1; }

extern "C" int later(int value) { return peer_raise(value) + 1; }
extern "C" int later(int) noexcept(false);

extern "C" int explicitly_nonthrowing(int value) noexcept { return peer_raise(value) + 1; }

extern "C" int eligible(int value) {
    if (value == 0)
        return 1;
    if (value > 7)
        return 8;
    return peer_raise(value) + 1;
}
