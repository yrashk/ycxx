extern "C" int amber_outer(int code);

int calls;
int destroyed;

struct PeerCleanup {
    ~PeerCleanup() { ++destroyed; }
};

extern "C" int amber_peer_step(int code) {
    ++calls;
    if (code == 7)
        throw 173;
    return code * 4 + 2;
}

int main(int argc, char**) {
    if (amber_outer(0) != 30 || amber_outer(4) != 41 || calls != 0)
        return 1;
    if (amber_outer(3) != 42 || amber_outer(-2) != -29 || calls != 2 || destroyed != 0)
        return 2;
    if (argc == 2)
        return 0;

    try {
        PeerCleanup cleanup;
        amber_outer(7);
        return 3;
    } catch (int value) {
        if (value != 173 || destroyed != 1 || calls != 3)
            return 4;
    } catch (...) {
        return 5;
    }
    return 0;
}
