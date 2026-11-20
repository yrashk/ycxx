#include "deleted-members.hpp"

void assign_deleted(DeletedMember& destination, const DeletedMember& source) {
    destination = source;  //! error
}

void assign_inaccessible(InaccessibleMember& destination, const InaccessibleMember& source) {
    destination = source;  //! error
}

void assign_reference(ReferenceMember& destination, const ReferenceMember& source) {
    destination = source;  //! error
}

void assign_const(ConstMember& destination, const ConstMember& source) {
    destination = source;  //! error
}
