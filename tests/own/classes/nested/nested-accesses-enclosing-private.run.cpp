// #306 [class.access.nest]/1: a nested class is a member and has the same access rights as any other member.
class O {
    int secret = 41;
    static int ssecret;
    class Priv { public: int z = 1; };
    friend class Other;
public:
    class I {
    public:
        int peek(const O& o) const { return o.secret + ssecret; }
        int priv() const { Priv p; return p.z; }
    };
};
int O::ssecret = 1;
int main() { O o; O::I i; return (i.peek(o) == 42 && i.priv() == 1) ? 0 : 1; }
