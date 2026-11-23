enum class Code : unsigned char { recorded = 19 };
struct Slot { Code state; };
Slot invalid{{37}};  //! error
