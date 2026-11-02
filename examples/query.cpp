#include "minicol/engine.hpp"
#include <iostream>

int main() {
    using namespace minicol;

    // Two rows per group makes skipping visible in a six-row example.
    // Normal files use 65,536 rows per group.
    write_file("build/demo.mcol",
               {{"day", Type::Int64}, {"region", Type::String}, {"amount", Type::Int64}},
               {std::vector<int64_t>{1, 2, 3, 4, 5, 6},
                std::vector<std::string>{"west", "east", "west", "west", "east", "west"},
                std::vector<int64_t>{10, 20, 30, 40, 50, 60}},
               {2, 2});

    Reader reader("build/demo.mcol");
    const std::vector<Predicate> filters = {
        {0, Op::Between, int64_t(3), int64_t(4)},
        {1, Op::Equal, std::string("west"), std::string("west")}};

    const auto count = reader.scan_all(filters);
    const auto total = reader.sum_int64(2, filters);
    std::cout << "Matched rows: " << count.matched << '\n'
              << "Amount sum: " << total.sum << '\n'
              << "Groups skipped: " << count.groups_skipped << " / " << reader.groups().size()
              << '\n';
}
