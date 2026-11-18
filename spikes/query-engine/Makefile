# Standalone build: ycxx's build skeleton (#4) does not exist yet. Any C++23 compiler will do.
CXX ?= clang++
CXXFLAGS ?= -std=c++23 -O2 -g -Wall -Wextra -pthread

build/spike: main.cpp engine.hpp toy.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ main.cpp

tsan: main.cpp engine.hpp toy.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -fsanitize=thread -o build/spike-tsan main.cpp

check: build/spike
	build/spike demo
	build/spike threads 16
	build/spike fuzz 300 8
	build/spike fuzz-bug 300 8

.PHONY: tsan check
