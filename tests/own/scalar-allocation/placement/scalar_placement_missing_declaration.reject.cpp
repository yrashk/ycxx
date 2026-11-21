int* place(void* address) {
    return new (address) int(9); //! error
}
