template<class T>
bool copy_array(T& value) {
    decltype(auto(value)) parens = auto(value);
    decltype(auto{value}) braces = auto{value};

    int** parens_type = &parens;
    int** braces_type = &braces;
    return *parens_type == &value[0] && *braces_type == &value[0];
}

int main() {
    int values[2] = {17, 21};
    return copy_array(values) ? 0 : 1;
}
