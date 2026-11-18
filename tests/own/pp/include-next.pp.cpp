// The GNU extension #include_next <h> (not in the draft; libycxx needs it): the search resumes
// in the directory after the one the current header was found in. next1/nx.h is found first
// and pulls in next2/nx.h.
// FLAGS: -Iinputs/next1 -Iinputs/next2
before;
#include <nx.h>
after;
