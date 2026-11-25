#include "deduced-callback.hpp"

int main() {
    return DeferredCallback<int>::invoke() == 61L ? 0 : 1;
}
