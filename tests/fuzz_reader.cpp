#include "minicol/engine.hpp"
#include "packed_fixture.hpp"
#include "structured_fuzz.hpp"
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
    const auto columns = structured_fuzz::columns();
    for (size_t c = 0; c < columns.size(); ++c)
        write_file((fs::path(argv[1]) / ("codec-" + std::to_string(c))).string(),
                   {{"v", c < 4 ? Type::Int64 : Type::String}}, {columns[c]}, {64, 64});
    write_file((fs::path(argv[1]) / "mixed").string(),
               {{"i", Type::Int64}, {"s", Type::String}}, {columns[1], columns[5]}, {64, 64});
    write_file((fs::path(argv[1]) / "empty").string(),
               {{"v", Type::Int64}}, {std::vector<int64_t>{}});
    // Rotate four-column windows so every codec leads mixed-column queries.
    // 17-row groups split runs and leave a ten-row tail; 64 leaves one row.
    for (uint32_t group_rows : {17u, 64u}) for (size_t first = 0; first < columns.size(); ++first) {
        std::vector<Field> schema;
        std::vector<Column> mixed;
        for (size_t j = 0; j < 4; ++j) {
            const auto& column = columns[(first + j) % columns.size()];
            schema.push_back({"c" + std::to_string(j), column.index() == 0 ? Type::Int64 : Type::String});
            mixed.push_back(column);
        }
        write_file((fs::path(argv[1]) / ("structured-" + std::to_string(group_rows) + "-" +
                   std::to_string(first))).string(), schema, mixed, {group_rows, 64});
    }
    // Explicit widths bypass adaptive writer fallback (especially width 64).
    // Nine rows include byte-crossing tails while fitting the harness limits.
    for (unsigned width = 0; width <= 64; ++width) {
        const auto bytes = packed_fixture::make(width, 9, INT64_MIN).file();
        std::ofstream out(fs::path(argv[1]) / ("packed-width-" + std::to_string(width)),
                          std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), std::streamsize(bytes.size()));
        if (!out) throw std::runtime_error("packed seed write failed");
    }
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
    require(reader.scan_all({}).matched == reader.rows());
    for (const auto& filters : structured_fuzz::queries(columns)) {
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
            if (filters.size() == 1) require(reader.scan(filters.front(), zones).matched == count);
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
