// [class.dtor]
struct Private {
    Private() {}
private:
    ~Private() {}
public:
    static void allowed() {
        Private local;
        (void)Private{};
    }
};

Private* pointer;
Private make();
extern Private object;
