// [basic.lookup.argdep] (#165)
namespace parameters {
struct Parameter {};
}

namespace results {
struct Result {};
}

using ParameterType = parameters::Parameter;
using ResultType = results::Result;
using Callback = ResultType (*)(ParameterType);

namespace parameters {
int via_parameter(Callback);
}

namespace results {
int via_result(ResultType (*)(ParameterType));
}

int check_parameter(ResultType (*callback)(ParameterType)) {
    return via_parameter(callback);
}

int check_result(Callback callback) {
    return via_result(callback);
}
