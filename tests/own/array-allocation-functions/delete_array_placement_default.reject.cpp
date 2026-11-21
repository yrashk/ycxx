struct Tag {};
const Tag tag{};
void operator delete[](void*, const Tag& = tag) noexcept;

void expression(int* pointer) {
    delete[] pointer;
}

void direct(void* pointer) {
    ::operator delete[](pointer); //! error
}
