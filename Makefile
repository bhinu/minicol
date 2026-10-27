CXX = c++
CXXFLAGS ?= -O3 -DNDEBUG -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS = -Iinclude
.PHONY: all test sanitize bench clean
all: build/test_engine build/benchmark
build:
	mkdir -p build
build/test_engine: src/engine.cpp tests/test_engine.cpp include/minicol/engine.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/engine.cpp tests/test_engine.cpp -o $@
build/benchmark: src/engine.cpp bench/benchmark.cpp include/minicol/engine.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/engine.cpp bench/benchmark.cpp -o $@
test: build/test_engine
	./build/test_engine
sanitize: | build
	$(CXX) $(CPPFLAGS) -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer src/engine.cpp tests/test_engine.cpp -o build/test_sanitize
	./build/test_sanitize
bench: build/benchmark
	./build/benchmark 1000000 7 build/data
clean:
	rm -rf build
