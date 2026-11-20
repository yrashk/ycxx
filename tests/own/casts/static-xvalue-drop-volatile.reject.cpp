int main() {
    volatile int object = 1;
    int&& alias = static_cast<int&&>(object);  //! error
    return alias;
}
