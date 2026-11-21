template<class T>
decltype(auto) copy_parens(T& value) {
    return auto(value);
}

template<class T>
decltype(auto) copy_braces(T& value) {
    return auto{value};
}

int main() {
    const int fixed = 17;
    volatile int observed = 19;
    decltype(auto) parens = copy_parens(fixed);
    decltype(auto) braces = copy_braces(observed);

    int* parens_type = &parens;
    int* braces_type = &braces;
    if (parens_type == &fixed || braces_type == &observed)
        return 1;
    return *parens_type == 17 && *braces_type == 19 ? 0 : 2;
}
