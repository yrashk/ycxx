template<unsigned Uses> requires (Uses <= 4)
struct Bounded;

Bounded<4>* allowed;
Bounded<5>* exceeds_limit; //! error

namespace storage {
template<class Buffer, unsigned Uses> requires (Uses <= sizeof(Buffer))
struct Limited;
}

storage::Limited<char[4], 4>* allowed_mixed;
storage::Limited<char[4], 5>* exceeds_mixed_limit; //! error
