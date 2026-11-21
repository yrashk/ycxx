using Unary = int (*)(int);

[[gnu::visibility("hidden")]] int hidden_from_decl(int);
int hidden_from_def(int);
int strong_hidden(int);
Unary default_addr();
Unary local_addr();
int overload(long);

int main() {
    if (hidden_from_decl(10) != 11)
        return 1;
    if (hidden_from_def(10) != 12)
        return 2;
    if (strong_hidden(10) != 13)
        return 3;
    if (default_addr()(10) != 14)
        return 4;
    if (local_addr()(10) != 15)
        return 5;
    if (overload(10L) != 17)
        return 6;
    return 0;
}
