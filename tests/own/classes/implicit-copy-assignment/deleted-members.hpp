#ifndef IMPLICIT_ASSIGNMENT_DELETED_MEMBERS_HPP
#define IMPLICIT_ASSIGNMENT_DELETED_MEMBERS_HPP

struct DeletedLeaf {
    const int value;
};

struct PrivateLeaf {
private:
    PrivateLeaf& operator=(const PrivateLeaf&);
};

struct DeletedMember {
    DeletedLeaf member;
};

struct InaccessibleMember {
    PrivateLeaf member;
};

struct ReferenceMember {
    int& member;
};

struct ConstMember {
    const int member;
};

#endif
