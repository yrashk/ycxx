template<class T>
void apply_fabsl(T input)
{
    (void)__builtin_fabsl(input);
}

void reject_pointer(int* input)
{
    apply_fabsl(input);
}
