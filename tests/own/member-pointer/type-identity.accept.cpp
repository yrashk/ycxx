// [dcl.mptr] (#238)
struct First;
struct Second;

int match(int*);
long match(int First::*);
short match(int Second::*);
char match(const int First::*);

using DataMember = int First::*;
extern const DataMember fixed;
extern int First::* const fixed;

int match_callable(void (First::*)(double) const & noexcept);
long match_callable(void (First::*)(double) const &);
short match_callable(void (First::*)(double) const && noexcept);
char match_callable(void (First::*)(double) & noexcept);
