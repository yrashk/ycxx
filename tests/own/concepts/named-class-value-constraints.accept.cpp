// [temp.names], [temp.constr.atomic]: no complete class definition is needed at the use.
template<unsigned Uses> requires (Uses <= 4)
struct Bounded;

Bounded<0>* none;
Bounded<4>* maximum;

namespace storage {
template<class Buffer, unsigned Uses> requires (Uses <= sizeof(Buffer))
struct Limited;
}

storage::Limited<char, 1>* single;
storage::Limited<char[4], 4>* selected;
