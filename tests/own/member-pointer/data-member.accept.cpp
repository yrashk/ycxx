// [dcl.mptr] (#238)
struct Forward;
struct Empty {};

using DataMember = int Forward::*;

void take_member(int Forward::* member);
void take_member(DataMember);

extern DataMember member;
extern int Forward::* member;
extern double Empty::* absent_member_type;
