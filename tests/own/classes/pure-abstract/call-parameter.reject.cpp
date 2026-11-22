#include "abstract.hpp"

void take(Abstract);
void run(Abstract& value) {
    take(value);  //! error
}
