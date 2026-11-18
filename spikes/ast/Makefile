# Standalone build: ycxx's build skeleton (#4) does not exist yet. Needs GCC 16 with -freflection.
CXX = g++-16
CXXFLAGS ?= -std=c++26 -freflection -O2 -Wall -Wextra

build/spike: spike.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ spike.cpp

build/spike-asan: spike.cpp
	@mkdir -p build
	$(CXX) -std=c++26 -freflection -O1 -g -fsanitize=address,undefined -DSPIKE_NK=24 -o $@ spike.cpp

check: build/spike build/spike-asan
	build/spike
	build/spike-asan

.PHONY: check
