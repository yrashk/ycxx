# Injected class names in ordinary member bodies

The ordinary unqualified type lookup for [#361](https://github.com/yrashk/ycxx/issues/361)
searches the complete class before its enclosing namespace. Astra checked the lookup and access
rules in [class.pre], [class.member.lookup], [class.access.base] and [class.qual].

The injected name denotes the class and has public member access. A direct member declaration
stops lookup on its branch. In a nonvirtual base graph, separate branches designate separate
subobjects. Their type declarations merge when they designate the same type. Competing types
make the lookup ambiguous. Lookup retains the base edges of each candidate so an accessible
route suffices when several subobjects supply the same type.

The parser probes for a type without reporting access or ambiguity errors. A committed
unqualified type use in an ordinary member body reports those errors at the type name. Class
lookup has already selected the declaration at that point. A block alias can hide it, and
`::B` explicitly selects the namespace declaration. Qualified and elaborated lookup have their
own rules, including the constructor interpretation of ordinary `B::B`.
