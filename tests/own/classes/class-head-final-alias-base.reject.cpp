class R;
using Alias = const R;
class R final {};
class R;
struct Rejected : Alias {}; //! error
