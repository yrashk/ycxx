// ι (#291): constinit applies only to variables with static or thread storage duration
// ([dcl.constinit]/1); a block variable without static is automatic.
int h() {
  constinit static int ok = 1;
  constinit int bad = 2; //! error
  return ok + bad;
}

int main() { return h(); }
