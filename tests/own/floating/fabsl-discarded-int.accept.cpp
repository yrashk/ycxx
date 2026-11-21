template<bool UseFabsl, class T>
int scalar_or_fabsl(T value)
{
    if constexpr (UseFabsl)
        return static_cast<int>(__builtin_fabsl(value));
    else
        return value + 1;
}

int main()
{
    return scalar_or_fabsl<false>(41) == 42 ? 0 : 1;
}
