enum class Phase { waiting, running, finished };

constexpr Phase advance(Phase phase) {
    switch (phase) {
    case Phase::waiting:
        return Phase::running;
    case Phase::running:
        return Phase::finished;
    case Phase::finished:
        return Phase::waiting;
    default:
        return phase;
    }
}

constexpr Phase initial = advance(Phase::waiting);
static_assert(initial == Phase::running);
static_assert(advance(initial) == Phase::finished);
static_assert(advance(Phase::finished) != Phase::finished);

int main(int argc, char**) {
    Phase input = argc == 1 ? Phase::waiting : Phase::running;
    Phase expected = argc == 1 ? Phase::running : Phase::finished;
    return advance(input) == expected ? 0 : 1;
}
