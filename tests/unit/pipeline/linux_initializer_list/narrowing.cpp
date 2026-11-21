#include <initializer_list>

void inspect(const std::initializer_list<int>&);

void reject_narrowing()
{
    inspect({17, 2.5});
}
