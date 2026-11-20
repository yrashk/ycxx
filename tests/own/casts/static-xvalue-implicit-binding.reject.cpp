int main() {
    int object = 1;
    int&& alias = object;  //! error
    return alias;
}
