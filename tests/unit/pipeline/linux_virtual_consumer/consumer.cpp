struct B {
    virtual int zeta(int);
    virtual int alpha(int);
};

B* next_receiver(bool use_derived);

int consume_pointer(B* object, int argument) {
    return object->alpha(argument);
}

int consume_reference(B& object, int argument) {
    return object.alpha(argument);
}

int consume_receiver(bool use_derived, int argument) {
    return next_receiver(use_derived)->alpha(argument);
}

int consume_qualified(B* object, int argument) {
    return object->B::alpha(argument);
}
