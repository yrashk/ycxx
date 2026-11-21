template<int N>
int copy_known_type() {
    decltype(auto(N)) parens = N;
    decltype(auto{N}) braces = N + 1;

    int* parens_type = &parens;
    int* braces_type = &braces;
    return *parens_type + *braces_type;
}

int main() {
    return copy_known_type<8>() == 17 && copy_known_type<12>() == 25 ? 0 : 1;
}
