// GCC also reports the reasons for implicit deletion at the class declarations.
// Keep each declaration and use on one marked line so both diagnostic styles locate the same case.
struct DeletedLeaf { const int value; }; struct DeletedMember { DeletedLeaf member; }; void assign_deleted(DeletedMember& destination, const DeletedMember& source) { destination = source; }  //! error
struct PrivateLeaf { private: PrivateLeaf& operator=(const PrivateLeaf&); }; struct InaccessibleMember { PrivateLeaf member; }; void assign_inaccessible(InaccessibleMember& destination, const InaccessibleMember& source) { destination = source; }  //! error
struct ReferenceMember { int& member; }; void assign_reference(ReferenceMember& destination, const ReferenceMember& source) { destination = source; }  //! error
struct ConstMember { const int member; }; void assign_const(ConstMember& destination, const ConstMember& source) { destination = source; }  //! error
