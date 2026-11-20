int main() {
    int* pointer = nullptr;
    int&& alias = static_cast<int&&>(pointer);  //! error
    return alias;
}
