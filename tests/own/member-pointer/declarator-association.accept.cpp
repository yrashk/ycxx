// [dcl.mptr] (#238)
struct Owner;

using DataMember = int Owner::*;
using OrdinaryPointer = int*;
using Array = int[3];

extern DataMember* pointer_to_member_pointer;
extern int Owner::** pointer_to_member_pointer;

extern OrdinaryPointer Owner::* pointer_member;
extern int* Owner::* pointer_member;

extern DataMember& member_pointer_reference;
extern int Owner::*& member_pointer_reference;

extern DataMember member_pointers[3];
extern int Owner::* member_pointers[3];

extern Array Owner::* array_member;
extern int (Owner::* array_member)[3];
