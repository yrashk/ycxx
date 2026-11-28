enum class Code : unsigned char { recorded = 19 };
void consume(Code);
void apply() { consume({37}); }  //! error
