#include <initializer_list>

namespace {

int evaluations;
int sequence;

int evaluate(int ordinal, int value)
{
    ++evaluations;
    sequence = sequence * 10 + ordinal;
    return value;
}

int inspect(const std::initializer_list<int>& values)
{
    if (evaluations != 3 || sequence != 123)
        return 1;
    if (values.size() != 3)
        return 2;
    const int* first = values.begin();
    if (first == nullptr || values.end() != first + 3)
        return 3;
    if (first[0] != 17 || first[1] != -4 || first[2] != 29)
        return 4;
    return 0;
}

int inspect_empty(const std::initializer_list<int>& values)
{
    return values.size() == 0 && values.begin() == values.end() ? 0 : 5;
}

const int* first_element(const std::initializer_list<int>& values)
{
    return values.begin();
}

}

int main()
{
    if (const int result = inspect({evaluate(1, 17), evaluate(2, -4),
                                    evaluate(3, 29)}))
        return result;
    if (*first_element({41, 43}) != 41)
        return 6;
    return inspect_empty({});
}
