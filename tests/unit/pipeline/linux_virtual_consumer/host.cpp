struct B;

B* provide_pointer(bool use_derived);
B& provide_reference(bool use_derived);
int receiver_count();

int consume_pointer(B* object, int argument);
int consume_reference(B& object, int argument);
int consume_receiver(bool use_derived, int argument);
int consume_qualified(B* object, int argument);

int main() {
    if (consume_pointer(provide_pointer(false), 7) != 207)
        return 1;
    if (consume_pointer(provide_pointer(true), 13) != 413)
        return 2;
    if (consume_reference(provide_reference(false), 17) != 217)
        return 3;
    if (consume_reference(provide_reference(true), 19) != 419)
        return 4;
    if (consume_receiver(true, 23) != 423)
        return 5;
    if (receiver_count() != 1)
        return 6;
    if (consume_qualified(provide_pointer(true), 29) != 239)
        return 7;
    return 0;
}
