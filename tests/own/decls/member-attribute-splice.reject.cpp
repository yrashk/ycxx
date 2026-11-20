// [dcl.attr.grammar]: a splice delimiter can enclose balanced attribute-argument tokens.
// XFAIL: clang Clang 23 does not tokenize the splice opener in this attribute argument.
// FLAGS: -freflection
struct Reading {
    [[probe::annotation([: unavailable_name)]] int value;  //! error
};
