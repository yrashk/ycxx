// XFAIL: clang,gcc The host compilers accept the spelling. ycxx diagnoses unsupported ABI.
struct Nontrivial {
  ~Nontrivial();
} __attribute__((trivial_abi)); //! error
