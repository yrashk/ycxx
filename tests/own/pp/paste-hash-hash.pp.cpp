// [cpp.concat]/4 (example): `# ## #` pastes two `#` tokens into a `##` token that is not an
// operator in the result; stringizing it via an intermediate macro keeps it as written.
#define hash_hash # ## #
#define mkstr(a) # a
#define in_between(a) mkstr(a)
#define join(c, d) in_between(c hash_hash d)
char p[] = join(x, y);
hash_hash;
