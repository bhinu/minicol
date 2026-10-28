CXX = c++
CXXFLAGS ?= -O3 -DNDEBUG -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS = -Iinclude
FUZZ_CXX ?= clang++
.PHONY: all test sanitize fuzz fuzz-seeds bench clean
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
build/fuzz_reader: src/engine.cpp tests/fuzz_reader.cpp include/minicol/engine.hpp | build
	$(FUZZ_CXX) $(CPPFLAGS) -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer src/engine.cpp tests/fuzz_reader.cpp -o $@
build/fuzz_seeds: src/engine.cpp tests/fuzz_reader.cpp include/minicol/engine.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DMINICOL_FUZZ_SEEDS src/engine.cpp tests/fuzz_reader.cpp -o $@
fuzz-seeds: build/fuzz_seeds
	./build/fuzz_seeds build/fuzz-corpus
fuzz: build/fuzz_reader fuzz-seeds
	mkdir -p build/fuzz-artifacts
	./build/fuzz_reader build/fuzz-corpus -seed=42 -runs=1000 -max_len=65536 -timeout=5 -artifact_prefix=build/fuzz-artifacts/
bench: build/benchmark
	./build/benchmark 1000000 7 build/data
clean:
	rm -rf build
