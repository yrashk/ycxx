// [class.friend], [dcl.fct.default]
struct FriendDefault {
private:
    ~FriendDefault();
    friend void consume(FriendDefault);
};
void consume(FriendDefault = FriendDefault{});

struct MemberDefault {
    static void consume(MemberDefault);
private:
    ~MemberDefault();
};
void MemberDefault::consume(MemberDefault = MemberDefault{}) {}
