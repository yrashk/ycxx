// [dcl.attr.grammar]: a splice delimiter can enclose balanced attribute-argument tokens.
// FLAGS: -freflection
struct Reading {
    [[probe::annotation([: unavailable_name :])]] int value;
};
