#include "minicol/engine.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

using namespace minicol;
namespace fs = std::filesystem;

#ifdef MINICOL_FUZZ_SEEDS
int main(int argc, char** argv) {
    if (argc != 2) return 1;
    fs::create_directories(argv[1]);
    std::vector<int64_t> plain(129), packed(129), runs(129), constant(129, -7);
    std::vector<std::string> text(129), dict(129), text_runs(129);
    for (size_t i = 0; i < plain.size(); ++i) {
        plain[i] = i % 2 ? INT64_MAX : INT64_MIN;
        packed[i] = int64_t(i % 31) - 15;
        runs[i] = (i / 32) % 2 ? INT64_MAX : INT64_MIN;
        text[i] = "unique-" + std::to_string(i) + std::string("\0tail", 5);
        dict[i] = "long-dictionary-label-" + std::to_string(i % 3);
        text_runs[i] = std::string(40, char('a' + i / 32));
    }
    const std::vector<Column> columns{plain, packed, runs, constant, text, dict, text_runs};
    for (size_t c = 0; c < columns.size(); ++c)
        write_file((fs::path(argv[1]) / ("codec-" + std::to_string(c))).string(),
                   {{"v", c < 4 ? Type::Int64 : Type::String}}, {columns[c]}, {64, 64});
    write_file((fs::path(argv[1]) / "mixed").string(),
               {{"i", Type::Int64}, {"s", Type::String}}, {packed, dict}, {64, 64});
    write_file((fs::path(argv[1]) / "empty").string(),
               {{"v", Type::Int64}}, {std::vector<int64_t>{}});
}
#else
namespace {
struct Scratch {
    fs::path dir;
    Scratch() {
        // Atomic directory creation isolates simultaneous fuzzer processes.
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned i = 0; ; ++i) {
            dir = fs::path("build") / ("fuzz-reader-" + std::to_string(stamp) + "-" + std::to_string(i));
            if (fs::create_directory(dir)) break;
        }
    }
    ~Scratch() { std::error_code ec; fs::remove_all(dir, ec); }
};
void require(bool condition) { if (!condition) std::abort(); }
Value at(const Column& column, size_t row) {
    return std::visit([&](const auto& v) -> Value { return v.at(row); }, column);
}
bool matches(const Value& value, const Predicate& p) {
    return p.op == Op::Equal ? value == p.low : value >= p.low && value <= p.high;
}
}
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 65536) return 0;
    static Scratch scratch;
    const auto path = (scratch.dir / "input.mcol").string();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data), std::streamsize(size));
        if (!out) throw std::runtime_error("fuzz input write failed");
    }
    // Only parsing/decoding errors are expected. Allocation failures, sanitizer
    // reports, and oracle mismatches must escape or crash the fuzz iteration.
    std::vector<Column> columns;
    bool trustworthy_zones = true;
    ReadLimits limits{4096, 4096, 64, 8192, UINT64_MAX, UINT64_MAX, 128};
    // At most 128 metadata entries and 8192 logical string bytes per chunk:
    // accepted inputs fit this cumulative cap, so oracle scans must succeed.
    limits.max_decoded_string_bytes_per_operation = 1048576;
    try {
        Reader reader(path, limits);
        if (reader.rows() > 256 || reader.schema().size() > 4) return 0;
        for (size_t c = 0; c < reader.schema().size(); ++c) {
            Column all = reader.schema()[c].type == Type::Int64
                       ? Column(std::vector<int64_t>{}) : Column(std::vector<std::string>{});
            for (size_t g = 0; g < reader.groups().size(); ++g) {
                const auto decoded = reader.read_column(g, c);
                const auto& m = reader.groups()[g][c];
                std::visit([&](const auto& values) {
                    require(values.size() == m.rows);
                    const auto bounds = std::minmax_element(values.begin(), values.end());
                    trustworthy_zones &= m.min <= Value(*bounds.first) && Value(*bounds.second) <= m.max;
                    auto& dst = std::get<std::decay_t<decltype(values)>>(all);
                    dst.insert(dst.end(), values.begin(), values.end());
                }, decoded);
            }
            require(std::visit([](const auto& v) { return v.size(); }, all) == reader.rows());
            columns.push_back(std::move(all));
        }
    } catch (const std::runtime_error&) { return 0; }

    // Outside the rejection handler: successfully decoded chunks must scan and
    // aggregate consistently with this independent, materialized scalar oracle.
    Reader reader(path, limits);
    std::vector<Predicate> predicates;
    require(reader.scan_all({}).matched == reader.rows());
    if (reader.rows()) {
        for (size_t c = 0; c < columns.size(); ++c) {
            const auto pivot = at(columns[c], size_t(reader.rows() / 2));
            predicates.push_back({c, Op::Equal, pivot, pivot});
            const auto bounds = std::visit([](const auto& v) {
                const auto b = std::minmax_element(v.begin(), v.end());
                return std::make_pair(Value(*b.first), Value(*b.second));
            }, columns[c]);
            predicates.push_back({c, Op::Between, bounds.first, bounds.second});
        }
    }
    for (size_t n = 0; n <= predicates.size(); ++n) {
        const std::vector<Predicate> filters(predicates.begin(), predicates.begin() + n);
        uint64_t count = 0;
        std::vector<uint64_t> sums(columns.size(), 0);
        for (size_t row = 0; row < reader.rows(); ++row) {
            bool hit = true;
            for (const auto& p : filters) hit &= matches(at(columns[p.column], row), p);
            if (!hit) continue;
            ++count;
            for (size_t c = 0; c < columns.size(); ++c)
                if (const auto* ints = std::get_if<std::vector<int64_t>>(&columns[c]))
                    sums[c] += uint64_t((*ints)[row]);
        }
        for (bool zones : {false, true}) {
            if (zones && !trustworthy_zones) continue;
            require(reader.scan_all(filters, zones).matched == count);
            for (size_t c = 0; c < columns.size(); ++c) {
                if (columns[c].index() != 0) continue;
                const auto result = reader.sum_int64(c, filters, zones);
                require(result.scan.matched == count && uint64_t(result.sum) == sums[c]);
            }
        }
    }
    return 0;
}
#endif
