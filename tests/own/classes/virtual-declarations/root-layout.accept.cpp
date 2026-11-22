struct Root {
    virtual int value() const;
};

constexpr auto root_size = sizeof(Root);

int main() {
    return root_size == 0;
}
