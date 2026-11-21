enum Mode { waiting, selected, finished };

struct Selection {
    static constexpr Mode value = sizeof(char) == 1 ? selected : finished;
};

static_assert(Selection::value == selected);

bool is_selected(Mode mode) {
    return mode == selected;
}

int main() {
    return is_selected(Selection::value) ? 0 : 1;
}
