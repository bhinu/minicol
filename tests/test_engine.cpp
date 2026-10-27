#include "minicol/engine.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace minicol;
namespace {
size_t checks = 0;
void check(bool ok, const std::string& message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
template<class F> void throws(F&& f, const std::string& message) {
    bool caught = false;
    try { f(); } catch (const std::exception&) { caught = true; }
    check(caught, message);
}
struct Scratch {
    fs::path dir;
    Scratch() {
        dir = fs::temp_directory_path() / ("minicol-tests-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir);
    }
    ~Scratch() { std::error_code ec; fs::remove_all(dir, ec); }
    std::string file(const std::string& name) const { return (dir / name).string(); }
};
bool satisfies(const Value& v, const Predicate& p) {
    if (p.op == Op::Equal) return v == p.low;
    return v >= p.low && v <= p.high;
}
uint64_t oracle(const Column& c, const Predicate& p) {
    return std::visit([&](const auto& values) {
        uint64_t n = 0;
        for (const auto& v : values) n += satisfies(Value(v), p);
        return n;
    }, c);
}
void scans(const Reader& reader, const std::vector<Column>& columns,
           const std::vector<Predicate>& predicates) {
    for (const auto& p : predicates) {
        const auto expected = oracle(columns.at(p.column), p);
        const auto full = reader.scan(p, false);
        const auto pruned = reader.scan(p, true);
        check(full.matched == expected, "unpruned scan differs from scalar oracle");
        check(pruned.matched == expected, "zone-map scan differs from scalar oracle");
        check(full.groups_read == reader.groups().size(), "full scan must read every group");
        check(full.groups_skipped == 0, "unpruned scan reports skipped groups");
        check(pruned.groups_read + pruned.groups_skipped == reader.groups().size(),
              "zone-map group accounting");
        uint64_t expected_bytes = reader.metadata_bytes();
        for (const auto& g : reader.groups()) expected_bytes += g.at(p.column).bytes;
        check(full.bytes_read == expected_bytes, "payload plus metadata byte accounting");
        check(pruned.bytes_read <= full.bytes_read, "pruned scan reads more bytes");
    }
}
void roundtrip(const std::string& path, const std::vector<Field>& schema,
               const std::vector<Column>& columns, uint32_t group_size = 1024,
               const std::vector<Predicate>& predicates = {}) {
    write_file(path, schema, columns, {group_size, 256});
    Reader reader(path);
    check(reader.schema().size() == schema.size(), "schema field count");
    for (size_t c = 0; c < schema.size(); ++c) {
        check(reader.schema()[c].name == schema[c].name, "schema field name");
        check(reader.schema()[c].type == schema[c].type, "schema field type");
    }
    size_t n = std::visit([](const auto& v) { return v.size(); }, columns.front());
    check(reader.rows() == n, "row count");
    check(reader.groups().size() == (n + group_size - 1) / group_size, "row-group tail count");
    for (size_t g = 0; g < reader.groups().size(); ++g) {
        size_t begin = g * group_size;
        size_t end = std::min(n, begin + group_size);
        for (size_t c = 0; c < columns.size(); ++c) {
            Column expected = std::visit([&](const auto& v) -> Column {
                using Vec = std::decay_t<decltype(v)>;
                return Vec(v.begin() + begin, v.begin() + end);
            }, columns[c]);
            check(reader.read_column(g, c) == expected, "column roundtrip");
            const auto& m = reader.groups()[g][c];
            check(m.rows == end - begin, "chunk row count");
            std::visit([&](const auto& v) {
                const auto bounds = std::minmax_element(v.begin(), v.end());
                check(m.min == Value(*bounds.first), "zone minimum");
                check(m.max == Value(*bounds.second), "zone maximum");
            }, expected);
        }
    }
    scans(reader, columns, predicates);
}
void encoding_tests(const Scratch& scratch) {
    std::mt19937_64 rng(7331);
    std::vector<int64_t> packed(8192), runs(8192);
    std::vector<std::string> strings(8192), string_runs(8192);
    for (size_t i = 0; i < packed.size(); ++i) {
        packed[i] = static_cast<int64_t>(rng() % 32) - 16;
        runs[i] = static_cast<int64_t>(i / 2048) - 2;
        strings[i] = "long-low-cardinality-value-" + std::to_string(rng() % 7);
        string_runs[i] = std::string(80, char('a' + i / 2048));
    }
    const std::vector<Field> schema{{"small", Type::Int64}, {"runs", Type::Int64},
                                    {"labels", Type::String}, {"text_runs", Type::String}};
    const std::vector<Column> columns{packed, runs, strings, string_runs};
    auto path = scratch.file("encodings.mc");
    roundtrip(path, schema, columns, 8192, {
        {0, Op::Equal, int64_t(-3), int64_t(-3)},
        {0, Op::Between, int64_t(-8), int64_t(6)},
        {1, Op::Equal, int64_t(0), int64_t(0)},
        {2, Op::Equal, std::string("long-low-cardinality-value-3"), std::string()},
        {2, Op::Equal, std::string("missing-inside-lexical-range"), std::string()},
        {2, Op::Between, std::string("long-low-cardinality-value-2"), std::string("long-low-cardinality-value-4")},
        {3, Op::Equal, std::string(80, 'c'), std::string()}});
    Reader r(path);
    check(r.groups()[0][0].encoding == Encoding::BitPacked, "small integers use bit packing");
    check(r.groups()[0][1].encoding == Encoding::RLE, "integer runs use RLE");
    check(r.groups()[0][2].encoding == Encoding::Dictionary, "low-cardinality strings use dictionary");
    check(r.groups()[0][3].encoding == Encoding::RLE, "string runs use RLE");
}
void edge_tests(const Scratch& scratch) {
    const std::vector<Field> schema{{"i", Type::Int64}, {"s", Type::String}};
    roundtrip(scratch.file("empty.mc"), schema,
              {std::vector<int64_t>{}, std::vector<std::string>{}}, 17,
              {{0, Op::Equal, int64_t(0), int64_t(0)}});
    const auto lo = std::numeric_limits<int64_t>::min();
    const auto hi = std::numeric_limits<int64_t>::max();
    std::vector<int64_t> integers{lo, hi, 0, -1, 1, lo + 1, hi - 1};
    std::vector<std::string> strings{"", "a", "z", std::string("a\0b", 3), "aa", "\xC3\xA9", "a"};
    roundtrip(scratch.file("extremes.mc"), schema, {integers, strings}, 3, {
        {0, Op::Between, lo, hi}, {0, Op::Equal, lo, lo}, {0, Op::Equal, hi, hi},
        {1, Op::Equal, std::string("a\0b", 3), std::string()},
        {1, Op::Between, std::string(""), std::string("az")}});
    roundtrip(scratch.file("single.mc"), schema,
              {std::vector<int64_t>{-7}, std::vector<std::string>{""}}, 1,
              {{0, Op::Equal, int64_t(-7), int64_t(-7)},
               {1, Op::Equal, std::string(""), std::string()}});
    roundtrip(scratch.file("constant.mc"), schema,
              {std::vector<int64_t>(3073, lo), std::vector<std::string>(3073, "same")}, 1024,
              {{0, Op::Equal, lo, lo}, {0, Op::Equal, hi, hi},
               {1, Op::Equal, std::string("same"), std::string()}});
    std::mt19937_64 rng(91527);
    for (size_t trial = 0; trial < 20; ++trial) {
        size_t n = 1 + rng() % 5000;
        std::vector<int64_t> ints(n);
        std::vector<std::string> text(n);
        for (size_t i = 0; i < n; ++i) {
            ints[i] = trial % 3 == 0 ? int64_t(rng()) : int64_t(rng() % 201) - 100;
            text[i] = trial % 2 == 0 ? std::to_string(rng() % 9) : std::to_string(rng());
        }
        roundtrip(scratch.file("random-" + std::to_string(trial) + ".mc"), schema,
                  {ints, text}, uint32_t(1 + rng() % 1300), {
            {0, Op::Equal, ints[n / 2], ints[n / 2]},
            {0, Op::Between, int64_t(-30), int64_t(40)},
            {1, Op::Equal, text[n / 3], std::string()},
            {1, Op::Between, std::string("2"), std::string("7")}});
    }
}
void pruning_tests(const Scratch& scratch) {
    std::vector<int64_t> sorted(8193);
    for (size_t i = 0; i < sorted.size(); ++i) sorted[i] = int64_t(i);
    const auto path = scratch.file("pruning.mc");
    roundtrip(path, {{"id", Type::Int64}}, {sorted}, 1024,
              {{0, Op::Between, int64_t(1024), int64_t(2047)},
               {0, Op::Equal, int64_t(-1), int64_t(-1)}});
    Reader r(path);
    auto hit = r.scan({0, Op::Between, int64_t(1024), int64_t(2047)});
    check(hit.groups_read == 1 && hit.groups_skipped == 8, "zone maps skip disjoint sorted groups");
    auto miss = r.scan({0, Op::Equal, int64_t(-1), int64_t(-1)});
    check(miss.groups_read == 0 && miss.bytes_read == r.metadata_bytes(), "all-pruned scan only reads metadata");
    auto full = r.scan({0, Op::Between, int64_t(0), int64_t(8192)}, false);
    check(full.batches == 9, "1024-value batch accounting including tail");
}
void payload_corruption_tests(const Scratch& scratch) {
    auto corrupt_payload = [&](const std::string& name, Column column, Type type,
                               Encoding expected, const std::function<void(std::string&, const ChunkMeta&)>& mutate) {
        auto path = scratch.file(name);
        write_file(path, {{"value", type}}, {column}, {8192, 256});
        Reader before(path);
        auto meta = before.groups().at(0).at(0);
        check(meta.encoding == expected, "corruption fixture uses requested codec");
        std::ifstream in(path, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), {});
        in.close();
        mutate(bytes, meta);
        { std::ofstream out(path, std::ios::binary); out.write(bytes.data(), std::streamsize(bytes.size())); }
        Reader broken(path);
        throws([&] { broken.read_column(0, 0); }, "malformed codec rejected on materialization");
        Value low = type == Type::Int64 ? Value(int64_t(0)) : Value(std::string("value"));
        throws([&] { broken.scan({0, Op::Equal, low, low}, false); }, "malformed codec rejected on unpruned scan");
    };
    std::vector<int64_t> long_runs(4096, 0);
    std::fill(long_runs.begin() + 2048, long_runs.end(), 1);
    corrupt_payload("bad-run.mc", long_runs, Type::Int64, Encoding::RLE,
                    [](auto& b, const auto& m) { for (size_t i = 0; i < 4; ++i) b[m.offset + i] = 0; });
    std::vector<int64_t> small(4096);
    for (size_t i = 0; i < small.size(); ++i) small[i] = int64_t(i % 31);
    corrupt_payload("bad-width.mc", small, Type::Int64, Encoding::BitPacked,
                    [](auto& b, const auto& m) { b[m.offset + 8] = 65; });
    std::vector<std::string> dict(4096);
    for (size_t i = 0; i < dict.size(); ++i) dict[i] = "long-dictionary-value-" + std::to_string(i % 3);
    corrupt_payload("bad-id.mc", dict, Type::String, Encoding::Dictionary,
                    [](auto& b, const auto& m) { for (size_t i = 0; i < 4; ++i) b[m.offset + m.bytes - 4 + i] = char(255); });
    std::vector<std::string> unique;
    for (size_t i = 0; i < 50; ++i) unique.push_back(std::to_string(i));
    corrupt_payload("bad-string.mc", unique, Type::String, Encoding::Plain,
                    [](auto& b, const auto& m) { for (size_t i = 0; i < 4; ++i) b[m.offset + i] = char(255); });
}
void invalid_tests(const Scratch& scratch) {
    const auto path = scratch.file("invalid.mc");
    throws([&] { write_file(path, {{"i", Type::Int64}}, {std::vector<int64_t>{1}}, {0, 1}); }, "reject zero row-group size");
    throws([&] { write_file(path, {{"i", Type::Int64}}, {std::vector<int64_t>{1}}, {1, 0}); }, "reject zero sample size");
    throws([&] { write_file(path, {{"i", Type::Int64}}, {std::vector<std::string>{"x"}}); }, "reject schema type mismatch");
    throws([&] { write_file(path, {{"i", Type::Int64}}, {}); }, "reject missing column");
    throws([&] { write_file(path, {{"a", Type::Int64}, {"b", Type::Int64}},
                            {std::vector<int64_t>{1}, std::vector<int64_t>{}}); }, "reject unequal column lengths");
    write_file(path, {{"i", Type::Int64}}, {std::vector<int64_t>{1, 2, 3}});
    Reader r(path);
    throws([&] { r.read_column(99, 0); }, "reject bad group");
    throws([&] { r.read_column(0, 99); }, "reject bad column");
    throws([&] { r.scan({99, Op::Equal, int64_t(1), int64_t(1)}); }, "reject bad predicate column");
    throws([&] { r.scan({0, Op::Equal, std::string("x"), std::string()}); }, "reject predicate type mismatch");
    check(r.scan({0, Op::Between, int64_t(3), int64_t(1)}).matched == 0, "inverted range is empty");
    std::ifstream in(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), {});
    for (size_t size : {size_t(0), size_t(3), bytes.size() / 2, bytes.size() - 1}) {
        auto corrupt = scratch.file("truncated-" + std::to_string(size));
        { std::ofstream out(corrupt, std::ios::binary); out.write(bytes.data(), std::streamsize(size)); }
        throws([&] { Reader bad(corrupt); }, "reject truncated file");
    }
    auto corrupt = scratch.file("bad-magic.mc");
    std::string bad = bytes;
    bad[0] ^= 0x7f;
    { std::ofstream out(corrupt, std::ios::binary); out.write(bad.data(), std::streamsize(bad.size())); }
    throws([&] { Reader broken(corrupt); }, "reject bad header magic");
    throws([&] { Reader missing(scratch.file("does-not-exist")); }, "reject missing file");
}
}
int main() {
    try {
        Scratch scratch;
        encoding_tests(scratch);
        edge_tests(scratch);
        pruning_tests(scratch);
        invalid_tests(scratch);
        payload_corruption_tests(scratch);
        std::cout << "PASS: " << checks << " checks (encodings, scalar oracle, edge cases, pruning, malformed files)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n';
        return 1;
    }
}
