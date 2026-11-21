enum class State { idle, ready };

State read_runtime() {
    return State::idle;
}

struct Settings {
    static constexpr State guarded = true ? State::ready : read_runtime();
    static constexpr State selected = false ? State::ready : read_runtime(); //! error
};
