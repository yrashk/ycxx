// #761 [basic.lookup.unqual], [basic.lookup.qual], [basic.lookup.elab].
namespace north { struct Station { enum class Mode { receive }; }; }
namespace south { struct Station { enum class Mode { send }; }; }

using namespace north;
using namespace south;

enum Station::Mode mode; //! error
