# Standalone build. HiGHS is built from source outside the repository: `make deps` clones the
# pinned commit into $(DEPS) and builds it with $(CXX) (same standard library as the spike).
ifeq ($(origin CXX),default)
CXX = g++-16
endif
DEPS ?= $(TMPDIR)/ycxx-egraph-deps
HIGHS_COMMIT = f3f113d230429a8c0014b12ab0dd1a3847e58c55
HIGHS = $(DEPS)/highs-install
CXXFLAGS ?= -std=c++26 -O2 -g -Wall -Wextra -pthread
SAN = -std=c++26 -O1 -g -Wall -Wextra -pthread -fsanitize=address,undefined -fno-sanitize-recover=undefined
INC = -isystem $(HIGHS)/include/highs
LIBS = -L$(HIGHS)/lib -lhighs -lz
SRC = main.cpp egraph.hpp extract.hpp

build/spike: $(SRC)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) -o $@ main.cpp $(LIBS)

build/spike-san: $(SRC)
	@mkdir -p build
	$(CXX) $(SAN) $(INC) -o $@ main.cpp $(LIBS)

deps:
	test -d $(DEPS)/HiGHS || git clone https://github.com/ERGO-Code/HiGHS.git $(DEPS)/HiGHS
	git -C $(DEPS)/HiGHS checkout -q $(HIGHS_COMMIT)
	CXX=$(CXX) cmake -S $(DEPS)/HiGHS -B $(DEPS)/highs-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
	  -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DFAST_BUILD=ON -DCMAKE_INSTALL_PREFIX=$(HIGHS)
	cmake --build $(DEPS)/highs-build && cmake --install $(DEPS)/highs-build

check: build/spike
	build/spike demo
	build/spike random 50 30 300
	build/spike determinism
	build/spike threads 8
	build/spike highs 1
	build/spike highs -1
	build/spike growth 100

san: build/spike-san
	build/spike-san demo
	build/spike-san random 10 30 300
	build/spike-san determinism
	build/spike-san threads 4
	build/spike-san highs 1

# GCC has no TSan on macOS/arm64: use Clang, with a HiGHS built by Clang (libc++), e.g.
#   make CXX=clang++ HIGHS=/path/to/highs-clang-install tsan
tsan: $(SRC)
	@mkdir -p build
	$(CXX) -std=c++2c -O1 -g -pthread -fsanitize=thread $(INC) -o build/spike-tsan main.cpp $(LIBS)
	build/spike-tsan threads 8

.PHONY: deps check san tsan
