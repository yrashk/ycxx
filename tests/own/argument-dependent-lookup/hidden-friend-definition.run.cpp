namespace telemetry {
class Packet {
    int reading;

public:
    Packet(int input) : reading(input) {}

    friend int score(const Packet& packet) {
        return packet.reading + adjustment;
    }

private:
    static constexpr int adjustment = 7;
};
}

int through_adl(int input) {
    telemetry::Packet packet(input);
    return score(packet);
}

namespace telemetry {
int score(const Packet& packet);
}

namespace delegated {
class B {
    int reading;

public:
    B(int input) : reading(input) {}

    friend int decode(const B& value);
};

struct A {
    friend int decode(const B& value) {
        return value.reading;
    }
};
}

int main() {
    if (through_adl(23) != 30) {
        return 1;
    }

    telemetry::Packet packet(37);
    if (telemetry::score(packet) != 44) {
        return 2;
    }

    delegated::B value(61);
    return decode(value) == 61 ? 0 : 3;
}
