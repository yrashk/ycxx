extern "C" int amber_peer_step(int code);

extern "C" int amber_inner(int code) {
    if (code == 0)
        return 19;
    int value = amber_peer_step(code);
    return value * 2 + 3;
}

extern "C" int amber_outer(int code) {
    if (code == 4)
        return 41;
    int value = amber_inner(code);
    if (code < 0)
        return value * 3 - 2;
    return value + 11;
}
