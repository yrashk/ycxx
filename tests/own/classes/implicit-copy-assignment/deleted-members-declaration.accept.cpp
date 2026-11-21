#include "deleted-members.hpp"

void declare_objects(int& reference) {
    DeletedMember deleted_member{{11}};
    InaccessibleMember inaccessible_member;
    ReferenceMember reference_member{reference};
    ConstMember const_member{17};
}
