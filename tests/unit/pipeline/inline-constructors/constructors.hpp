#pragma once

// These three constructors can be defined in both translation units ([dcl.inline], [basic.def.odr]).
struct InClass {
    int value;
    InClass(int input) : value(input) {}
};

struct OnDefinition {
    int value;
    OnDefinition(int input);
};

inline OnDefinition::OnDefinition(int input) : value(input + 2) {}

struct OnDeclaration {
    int value;
    inline OnDeclaration(int input);
};

OnDeclaration::OnDeclaration(int input) : value(input + 3) {}

namespace {
struct LocalCtor {
    int value;
    LocalCtor(int input) : value(input + LOCAL_CTOR_OFFSET) {}
};
}

struct OutOfLine {
    int value;
    OutOfLine(int input);
};

bool check_peer(const InClass& caller);
