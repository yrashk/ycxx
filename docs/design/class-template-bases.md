# A single nonvirtual base in a class template

Astra reviewed the initial scope on [#308](https://github.com/yrashk/ycxx/issues/308#issuecomment-7054829849).
A primary class template can have one nonvirtual, nonpolymorphic direct base. An ordinary base name
binds at the class definition, where its class must be complete. A base that depends on a
template argument resolves when a specialization is required to be complete. A pointer to
the specialization alone leaves its definition uninstantiated. This covers the
`Bool<__is_same(T, U)>` base used by the pinned libycxx headers. [class.derived] [temp.inst]

Keep the canonical base type and access in `ClassDef::bases`. The defining item supplies the
token offsets for diagnostics and for checking written dependent alias arguments. Structural
substitution handles a canonical dependent type. Where the parser records checks that the
canonical type can erase, re-read the base type with arguments bound, lexical lookup at the
definition, and dependent completeness at the demanding point. An alias may erase a written
argument from its result type even when that argument must still be valid. [temp.alias]

Validate the substituted class before analysing its members or synthesizing special members.
The direct base check also applies the ordinary virtual-declaration guard to the result of
substitution. [#309](https://github.com/yrashk/ycxx/issues/309)
The instantiated class publishes its complete record with an ordinary `BaseRec`, which
supplies layout, access, and conversions to the existing nonvirtual inheritance paths. The
edge records whether it was dependent in the pattern. Unqualified lookup from that pattern's
member expressions and types skips the dependent direct edge during replay. Qualified and
object-member lookup on the concrete specialization use the resolved graph. [temp.dep]

Qualified inherited static lookup searches the derived class first and returns the original
base member entity, so values, addresses and references designate the same object as a lookup
through the base. Member access and the inheritance path both apply. A static member needs an
accessible path and can be shared through several base subobjects. [class.member.lookup]
[class.access.base]

The unit owns both the pattern and the instantiated records. The existing front-end query
rebuilds them from the preprocessed inputs and exports each concrete base edge in the layout
shape. The warm edit changes the selected base while keeping the derived specialization and
member spelling fixed.

Base packs, multiple direct bases in class templates, and base clauses on explicit and partial
specializations remain in [#308](https://github.com/yrashk/ycxx/issues/308). Inheriting constructors
follow [#136](https://github.com/yrashk/ycxx/issues/136). Final and polymorphic class support
follows [#309](https://github.com/yrashk/ycxx/issues/309), and virtual bases follow
[#310](https://github.com/yrashk/ycxx/issues/310).
