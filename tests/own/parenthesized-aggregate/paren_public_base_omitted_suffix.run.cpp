struct B {
    int base_value;
};

struct D : public B {
    int supplied;
    int omitted;
};

int main() {
    B source{17};
    int calls = 0;
    D result(source, (++calls, source.base_value = 43, 29));

    if (result.base_value != 17 || source.base_value != 43)
        return 1;
    if (result.supplied != 29 || result.omitted != 0)
        return 2;
    return calls == 1 ? 0 : 3;
}
