extern int* peer_operand(int selector);

extern "C" void release_from_peer(int selector) {
    delete[] peer_operand(selector);
}
