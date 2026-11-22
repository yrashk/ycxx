// [dcl.attr.grammar]: the attribute argument takes balanced tokens.
struct Reading { [[nodiscard("use the reading"]] int value() const; };  //! error
