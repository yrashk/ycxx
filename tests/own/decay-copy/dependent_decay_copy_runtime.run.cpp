template<class T>
auto copy_parens(T value) {
    return auto(value);
}

template<class T>
auto copy_braces(T value) {
    return auto{value};
}

template<class T>
bool copy_once(T initial) {
    T value = initial;
    T parens = auto(value++);
    T braces = auto{value++};
    return parens == initial && braces == initial + 1 && value == initial + 2;
}

int main() {
    int storage = 7;
    if (copy_parens(13) != 13 || copy_braces(19) != 19)
        return 1;

    int* parens = copy_parens(&storage);
    int* braces = copy_braces(&storage);
    if (parens != &storage || braces != &storage)
        return 2;

    return copy_once(23) && copy_once(29L) ? 0 : 3;
}
