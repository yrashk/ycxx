// The peer of peer.run.cpp, compiled by Clang.
static int compute() { return 42; }
int value = compute();
int peer_value() { return value; }
