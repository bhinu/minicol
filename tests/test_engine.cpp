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
uint64_t conjunction_oracle(const std::vector<Column>& columns,
                            const std::vector<Predicate>& predicates) {
    const auto rows = std::visit([](const auto& v) { return v.size(); }, columns.front());
    uint64_t matched = 0;
    for (size_t row = 0; row < rows; ++row) {
        bool hit = true;
        for (const auto& p : predicates) {
            const auto v = std::visit([&](const auto& values) -> Value { return values[row]; },
                                      columns.at(p.column));
            if (!satisfies(v, p)) hit = false;
        }
        matched += hit;
    }
    return matched;
}
uint64_t sum_oracle(const std::vector<Column>& columns, size_t projection,
                    const std::vector<Predicate>& predicates) {
    uint64_t sum = 0;
    const auto& values = std::get<std::vector<int64_t>>(columns.at(projection));
    for (size_t row = 0; row < values.size(); ++row) {
        bool hit = true;
        for (const auto& p : predicates) {
            const auto v = std::visit([&](const auto& column) -> Value { return column[row]; },
                                      columns.at(p.column));
            if (!satisfies(v, p)) hit = false;
        }
        if (hit) sum += static_cast<uint64_t>(values[row]);
    }
    return sum;
}
void conjunctions(const Reader& reader, const std::vector<Column>& columns,
                  const std::vector<Predicate>& predicates) {
    const auto expected = conjunction_oracle(columns, predicates);
    for (bool zones : {false, true}) {
        const auto result = reader.scan_all(predicates, zones);
        check(result.matched == expected, "AND scan differs from scalar row oracle");
        for (size_t c = 0; c < columns.size(); ++c) {
            if (columns[c].index() != 0) continue;
            const auto aggregate = reader.sum_int64(c, predicates, zones);
            check(aggregate.scan.matched == expected, "sum count differs from scalar row oracle");
            check(uint64_t(aggregate.sum) == sum_oracle(columns, c, predicates),
                  "projected sum differs from scalar row oracle");
            auto reverse = predicates;
            std::reverse(reverse.begin(), reverse.end());
            check(reader.sum_int64(c, reverse, zones).sum == aggregate.sum,
                  "sum independent of predicate order");
        }
        if (predicates.empty()) {
            check(result.bytes_read == reader.metadata_bytes() && result.groups_read == 0 &&
                  result.groups_skipped == 0 && result.batches == 0, "empty AND reads metadata only");
            continue;
        }
        check(result.groups_read + result.groups_skipped == reader.groups().size(), "AND group accounting");
        if (!zones) {
            check(result.groups_read == reader.groups().size(), "unpruned AND visits every group");
            uint64_t batches = 0;
            for (const auto& g : reader.groups()) batches += (uint64_t(g.front().rows) + 1023) / 1024;
            check(result.batches == batches, "AND logical batches counted once per group");
        }
        auto reversed = predicates;
        std::reverse(reversed.begin(), reversed.end());
        check(reader.scan_all(reversed, zones).matched == expected, "AND order independence");
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
    conjunctions(reader, columns, {});
    for (size_t count = 1; count <= predicates.size(); ++count)
        conjunctions(reader, columns, {predicates.begin(), predicates.begin() + count});
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
void conjunction_tests(const Scratch& scratch) {
    const size_t n = 8193;
    std::vector<int64_t> small(n), plain(n), runs(n);
    std::vector<std::string> dict(n), text(n), text_runs(n);
    for (size_t i = 0; i < n; ++i) {
        small[i] = int64_t(i % 31);
        plain[i] = i % 2 ? std::numeric_limits<int64_t>::max() : std::numeric_limits<int64_t>::min();
        runs[i] = int64_t(i / 1100);
        dict[i] = "long-dictionary-label-" + std::to_string(i % 7);
        text[i] = "unique-" + std::to_string(i) + std::string("\0tail", 5);
        text_runs[i] = std::string(50, char('a' + i / 1100));
    }
    const std::vector<Column> columns{small, plain, runs, dict, text, text_runs};
    const auto path = scratch.file("and.mc");
    roundtrip(path, {{"small", Type::Int64}, {"plain", Type::Int64}, {"runs", Type::Int64},
                    {"dict", Type::String}, {"text", Type::String}, {"text_runs", Type::String}},
              columns, 4096);
    Reader r(path);
    const std::vector<Encoding> encodings{Encoding::BitPacked, Encoding::Plain, Encoding::RLE,
                                         Encoding::Dictionary, Encoding::Plain, Encoding::RLE};
    for (size_t c = 0; c < encodings.size(); ++c)
        check(r.groups()[0][c].encoding == encodings[c], "AND fixture encoding");
    // Every pair of codecs, including run boundaries and batch/group tails.
    const std::vector<Predicate> filters{
        {0, Op::Between, int64_t(3), int64_t(25)},
        {1, Op::Equal, std::numeric_limits<int64_t>::max(), std::string()},
        {2, Op::Between, int64_t(1), int64_t(6)},
        {3, Op::Between, std::string("long-dictionary-label-2"), std::string("long-dictionary-label-5")},
        {4, Op::Between, std::string("unique-200"), std::string("unique-8000z")},
        {5, Op::Between, std::string(50, 'b'), std::string(50, 'g')}};
    for (const auto& a : filters) for (const auto& b : filters) conjunctions(r, columns, {a, b});
    conjunctions(r, columns, filters);
    conjunctions(r, columns, {{4, Op::Equal, text.back(), std::string()},
                              {0, Op::Equal, small.back(), int64_t(0)}});
    const Predicate missing{0, Op::Equal, int64_t(999), int64_t(999)};
    auto skipped = r.scan_all({filters[3], missing});
    check(skipped.groups_read == 0 && skipped.groups_skipped == r.groups().size() &&
          skipped.bytes_read == r.metadata_bytes(), "any AND zone can prune before any payload");
    auto stopped = r.scan_all({missing, filters[3]}, false);
    uint64_t first_bytes = r.metadata_bytes();
    for (const auto& g : r.groups()) first_bytes += g[0].bytes;
    check(stopped.matched == 0 && stopped.bytes_read == first_bytes, "empty selection avoids later columns");
    const std::vector<Predicate> all{{0, Op::Between, int64_t(0), int64_t(30)},
                                     {3, Op::Between, std::string(), std::string("zz")}};
    uint64_t two_column_bytes = r.metadata_bytes();
    for (const auto& g : r.groups()) two_column_bytes += g[0].bytes + g[3].bytes;
    auto all_result = r.scan_all(all, false);
    check(all_result.matched == n && all_result.bytes_read == two_column_bytes,
          "AND reads exactly its predicate columns");
    std::mt19937_64 rng(87291);
    for (size_t trial = 0; trial < 40; ++trial) {
        std::vector<Predicate> random;
        for (size_t j = 0; j < 2 + trial % 5; ++j) {
            const size_t c = rng() % columns.size();
            auto at = [&](size_t row) { return std::visit([&](const auto& v) -> Value { return v[row]; }, columns[c]); };
            auto low = at(rng() % n), high = at(rng() % n);
            if (high < low) std::swap(low, high);
            random.push_back({c, trial % 3 == 0 ? Op::Equal : Op::Between, low, high});
        }
        conjunctions(r, columns, random);
    }
    auto repeated = r.scan_all({filters[0], filters[0]}, false);
    check(repeated.bytes_read == first_bytes, "repeated column read only once");
    const std::vector<Predicate> incompatible{{0, Op::Equal, int64_t(3), int64_t(0)},
                                             {0, Op::Equal, int64_t(4), int64_t(0)}};
    conjunctions(r, columns, incompatible);
    conjunctions(r, columns, {{0, Op::Between, int64_t(9), int64_t(2)}, filters[3]});
    throws([&] { r.scan_all({missing, {99, Op::Equal, int64_t(0), int64_t(0)}}); },
           "validate later column before pruning");
    throws([&] { r.scan_all({missing, {1, Op::Between, int64_t(0), std::string()}}); },
           "validate later bound type before pruning");
    throws([&] { r.scan_all({missing, {1, static_cast<Op>(99), int64_t(0), int64_t(0)}}); },
           "validate later operation before pruning");
}
void payload_corruption_tests(const Scratch& scratch) {
    auto corrupt_payload = [&](const std::string& name, Column column, Type type,
                               Encoding expected, const std::function<void(std::string&, const ChunkMeta&)>& mutate) {
        auto path = scratch.file(name);
        const auto n = std::visit([](const auto& values) { return values.size(); }, column);
        write_file(path, {{"value", type}, {"guard", Type::Int64}},
                   {column, std::vector<int64_t>(n, 0)}, {8192, 256});
        Reader before(path);
        auto meta = before.groups().at(0).at(0);
        check(meta.encoding == expected, "corruption fixture uses requested codec");
        std::ifstream in(path, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), {});
        in.close();
        mutate(bytes, meta);
        { std::ofstream out(path, std::ios::binary); out.write(bytes.data(), std::streamsize(bytes.size())); }
        for (uint64_t cap : {UINT64_MAX, UINT64_MAX - 1}) {
            Reader broken(path, {UINT64_MAX, UINT64_MAX, UINT32_MAX, cap});
            throws([&] { broken.read_column(0, 0); }, "malformed codec rejected on materialization");
            Value low = type == Type::Int64 ? Value(int64_t(0)) : Value(std::string("value"));
            throws([&] { broken.scan({0, Op::Equal, low, low}, false); }, "malformed codec rejected on unpruned scan");
            throws([&] { broken.scan_all({{1, Op::Equal, int64_t(0), int64_t(0)},
                                         {0, Op::Equal, low, low}}, false); },
                   "malformed codec rejected in AND selection path");
            const Predicate guard{1, Op::Equal, int64_t(0), int64_t(0)};
            if (type == Type::Int64)
                throws([&] { broken.sum_int64(0, {guard}, false); },
                       "malformed projected integer payload rejected");
            throws([&] { broken.sum_int64(1, {{0, Op::Equal, low, low}}, false); },
                   "malformed aggregate predicate payload rejected");
            const Predicate absent{1, Op::Equal, int64_t(1), int64_t(1)};
            for (bool zones : {false, true}) {
                const auto empty = broken.sum_int64(type == Type::Int64 ? 0 : 1, {absent}, zones);
                check(empty.sum == 0 && empty.scan.matched == 0,
                      "empty aggregate avoids malformed unused payload");
            }
        }
    };
    std::vector<int64_t> long_runs(4096, 0);
    std::fill(long_runs.begin() + 2048, long_runs.end(), 1);
    corrupt_payload("bad-run.mc", long_runs, Type::Int64, Encoding::RLE,
                    [](auto& b, const auto& m) { for (size_t i = 0; i < 4; ++i) b[m.offset + i] = 0; });
    corrupt_payload("bad-string-run.mc", std::vector<std::string>(4096, "run-value"),
                    Type::String, Encoding::RLE,
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
void aggregate_tests(const Scratch& scratch) {
    const auto lo = std::numeric_limits<int64_t>::min();
    const auto hi = std::numeric_limits<int64_t>::max();
    const auto path = scratch.file("aggregate.mc");
    const std::vector<Column> columns{std::vector<int64_t>{hi, 1, lo, -1, 4},
                                      std::vector<int64_t>{0, 0, 1, 1, 2},
                                      std::vector<std::string>{"a", "a", "b", "b", "c"}};
    write_file(path, {{"value", Type::Int64}, {"guard", Type::Int64}, {"text", Type::String}},
               columns, {2, 2});
    Reader r(path);
    const Predicate positive{1, Op::Equal, int64_t(0), int64_t(0)};
    const Predicate negative{1, Op::Equal, int64_t(1), int64_t(0)};
    check(r.sum_int64(0, {positive}).sum == lo, "positive sum overflow wraps to INT64_MIN");
    check(r.sum_int64(0, {negative}).sum == hi, "negative sum overflow wraps to INT64_MAX");
    check(r.sum_int64(0).sum == 3, "sum wraps across row groups");
    conjunctions(r, columns, {});
    conjunctions(r, columns, {positive});
    const auto all = r.sum_int64(0);
    uint64_t projected_bytes = r.metadata_bytes(), two_bytes = r.metadata_bytes();
    uint64_t guard_bytes = r.metadata_bytes();
    for (const auto& g : r.groups()) {
        projected_bytes += g[0].bytes;
        guard_bytes += g[1].bytes;
        two_bytes += g[0].bytes + g[1].bytes;
    }
    check(all.scan.bytes_read == projected_bytes && all.scan.groups_read == 3 &&
          all.scan.groups_skipped == 0 && all.scan.batches == 3,
          "unfiltered sum reads only projection with group and batch accounting");
    const Predicate shared{0, Op::Between, lo, hi};
    check(r.sum_int64(0, {shared, shared}, false).scan.bytes_read == projected_bytes,
          "projection shared with repeated predicates read once");
    const Predicate every{1, Op::Between, int64_t(0), int64_t(2)};
    check(r.sum_int64(0, {every}, false).scan.bytes_read == two_bytes,
          "sum reads exactly predicate and projection columns");
    const Predicate missing{1, Op::Equal, int64_t(9), int64_t(0)};
    const auto pruned = r.sum_int64(0, {missing});
    check(pruned.sum == 0 && pruned.scan.matched == 0 && pruned.scan.groups_read == 0 &&
          pruned.scan.groups_skipped == 3 && pruned.scan.bytes_read == r.metadata_bytes() &&
          pruned.scan.batches == 0, "all-pruned sum reads metadata only");
    check(r.sum_int64(0, {missing}, false).scan.bytes_read == guard_bytes,
          "empty selection skips projected chunk without zones");
    const auto partial = r.sum_int64(0, {positive});
    check(partial.scan.groups_read == 1 && partial.scan.groups_skipped == 2 &&
          partial.scan.batches == 1 && partial.scan.bytes_read == r.metadata_bytes() +
          r.groups()[0][0].bytes + r.groups()[0][1].bytes, "pruned aggregate exact counters");
    throws([&] { r.sum_int64(99, {missing}); }, "validate projection column before pruning");
    throws([&] { r.sum_int64(2, {missing}); }, "reject string sum before pruning");
    throws([&] { r.sum_int64(0, {missing, {99, Op::Equal, int64_t(0), int64_t(0)}}); },
           "validate aggregate predicate column before pruning");
    throws([&] { r.sum_int64(0, {missing, {0, Op::Between, int64_t(0), std::string()}}); },
           "validate aggregate predicate types before pruning");
    throws([&] { r.sum_int64(0, {missing, {0, static_cast<Op>(99), int64_t(0), int64_t(0)}}); },
           "validate aggregate predicate operation before pruning");
}
// Build minimal v1 files independently of the writer for dimension attacks.
void append_le(std::string& bytes, uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) bytes.push_back(char(value >> (8 * i)));
}
std::string dimension_fixture(uint32_t rows, Encoding encoding, const std::string& payload) {
    std::string bytes = "MINICOL1";
    append_le(bytes, 1, 4);
    append_le(bytes, rows, 4);
    bytes += payload;
    std::string footer;
    append_le(footer, rows, 8);
    append_le(footer, 1, 4); // one int64 column
    append_le(footer, 1, 4); // one group
    append_le(footer, 1, 4);
    footer += 'v';
    footer += char(Type::Int64);
    append_le(footer, rows, 4);
    footer += char(encoding);
    append_le(footer, 16, 8);
    append_le(footer, payload.size(), 8);
    append_le(footer, 0, 8);
    append_le(footer, 0, 8);
    bytes += footer;
    append_le(bytes, footer.size(), 8);
    bytes += "MCOLEND1";
    return bytes;
}
void bounded_input_tests(const Scratch& scratch) {
    const auto path = scratch.file("bounded.mc");
    const auto lo = std::numeric_limits<int64_t>::min();
    const auto hi = std::numeric_limits<int64_t>::max();
    std::vector<int64_t> plain(513), packed(513), runs(513);
    std::vector<std::string> text(513), dict(513), text_runs(513);
    for (size_t i = 0; i < plain.size(); ++i) {
        plain[i] = i % 2 ? hi : lo;
        packed[i] = int64_t(i % 31);
        runs[i] = (i / 128) % 2 ? hi : lo;
        text[i] = "unique-" + std::to_string(i);
        dict[i] = "long-dictionary-entry-" + std::to_string(i % 3);
        text_runs[i] = std::string(40, char('a' + i / 128));
    }
    const std::vector<Column> columns{plain, packed, runs, text, dict, text_runs};
    write_file(path, {{"plain", Type::Int64}, {"packed", Type::Int64}, {"runs", Type::Int64},
                      {"text", Type::String}, {"dict", Type::String}, {"text_runs", Type::String}},
               columns, {256, 256});
    Reader unrestricted(path);
    const std::vector<Encoding> expected{Encoding::Plain, Encoding::BitPacked, Encoding::RLE,
                                          Encoding::Plain, Encoding::Dictionary, Encoding::RLE};
    uint64_t chunk_limit = 0;
    for (const auto& group : unrestricted.groups())
        for (const auto& m : group) chunk_limit = std::max(chunk_limit, m.bytes);
    const ReadLimits limits{unrestricted.metadata_bytes() - 32, chunk_limit, 256};
    Reader bounded(path, limits);
    for (size_t c = 0; c < columns.size(); ++c) {
        check(bounded.groups()[0][c].encoding == expected[c], "bounded fixture codec");
        for (size_t g = 0; g < bounded.groups().size(); ++g) {
            const auto begin = g * 256, end = std::min(begin + 256, plain.size());
            const Column wanted = std::visit([&](const auto& v) -> Column {
                using Vec = std::decay_t<decltype(v)>;
                return Vec(v.begin() + begin, v.begin() + end);
            }, columns[c]);
            check(bounded.read_column(g, c) == wanted, "bounded decoding matches source values");
        }
    }
    const std::vector<Predicate> filters{{0, Op::Equal, hi, lo},
                                         {4, Op::Equal, dict[1], std::string()}};
    scans(bounded, columns, filters);
    conjunctions(bounded, columns, filters);
    conjunctions(bounded, columns, {});
    // Operation budgets accumulate across columns/groups, independently of per-chunk caps.
    auto with_budget = [&](uint64_t bytes) {
        return Reader(path, {UINT64_MAX, UINT64_MAX, UINT32_MAX, UINT64_MAX, bytes});
    };
    auto budget_rejects = [&](const std::function<void()>& operation) {
        bool rejected = false;
        try { operation(); }
        catch (const std::runtime_error& e) {
            rejected = std::string(e.what()) == "operation payload byte limit exceeded";
        }
        check(rejected, "operation rejects with payload budget error");
    };
    auto boundary = [&](uint64_t bytes, const std::function<void(const Reader&)>& operation) {
        check(bytes > 0, "nonempty operation budget fixture");
        const auto exact = with_budget(bytes);
        operation(exact);
        operation(exact); // Each call gets a fresh budget.
        const auto below = with_budget(bytes - 1);
        budget_rejects([&] { operation(below); });
        check(below.scan_all({}).matched == plain.size(), "failed call leaves reader usable");
    };
    for (size_t c = 0; c < columns.size(); ++c) {
        uint64_t bytes = 0;
        for (size_t g = 0; g < bounded.groups().size(); ++g) {
            const auto& m = bounded.groups()[g][c];
            bytes += m.bytes;
            const auto wanted = bounded.read_column(g, c);
            boundary(m.bytes, [&](const Reader& r) {
                check(r.read_column(g, c) == wanted, "payload budget roundtrip matches source-checked chunk");
            });
        }
        const Value low = c < 3 ? Value(lo) : Value(std::string());
        const Value high = c < 3 ? Value(hi) : Value(std::string(1, char(127)));
        const Predicate all{c, Op::Between, low, high};
        boundary(bytes, [&](const Reader& r) {
            for (bool zones : {false, true}) {
                const auto result = r.scan(all, zones);
                check(result.matched == oracle(columns[c], all), "budgeted count matches scalar oracle");
                check(result.bytes_read == r.metadata_bytes() + bytes, "budget excludes metadata");
                check(r.scan_all({all, all}, zones).matched == oracle(columns[c], all),
                      "repeated predicates charge a chunk once");
            }
        });
        if (c < 3) boundary(bytes, [&](const Reader& r) {
            for (const auto& predicates : std::vector<std::vector<Predicate>>{{}, {all, all}}) {
                const auto result = r.sum_int64(c, predicates, false);
                check(uint64_t(result.sum) == sum_oracle(columns, c, predicates),
                      "budgeted shared/unfiltered projection matches scalar oracle");
                check(result.scan.bytes_read == r.metadata_bytes() + bytes,
                      "shared projection charges encoded bytes once");
            }
        });
    }
    const std::vector<Predicate> budget_filters{
        {0, Op::Between, lo, hi},
        {4, Op::Between, std::string(), std::string(1, char(127))}};
    uint64_t two_columns = 0, three_columns = 0;
    for (const auto& g : bounded.groups()) {
        two_columns += g[0].bytes + g[4].bytes;
        three_columns += g[0].bytes + g[4].bytes + g[1].bytes;
    }
    boundary(two_columns, [&](const Reader& r) {
        check(r.scan_all(budget_filters, false).matched == conjunction_oracle(columns, budget_filters),
              "multi-column budgeted AND matches scalar oracle");
        check(uint64_t(r.sum_int64(0, budget_filters, false).sum) == sum_oracle(columns, 0, budget_filters),
              "AND with shared projection matches scalar oracle");
    });
    boundary(three_columns, [&](const Reader& r) {
        check(uint64_t(r.sum_int64(1, budget_filters, false).sum) == sum_oracle(columns, 1, budget_filters),
              "separate projection charged after predicates");
    });
    const Predicate tail_pruned{1, Op::Equal, int64_t(30), int64_t(0)};
    uint64_t pruned_bytes = bounded.groups()[0][1].bytes + bounded.groups()[1][1].bytes;
    boundary(pruned_bytes, [&](const Reader& r) {
        check(r.scan(tail_pruned).matched == oracle(columns[1], tail_pruned),
              "partial zone pruning charges only visited groups");
        check(uint64_t(r.sum_int64(1, {tail_pruned}).sum) ==
                  sum_oracle(columns, 1, {tail_pruned}),
              "partially pruned sum matches scalar oracle");
    });
    budget_rejects([&] { with_budget(pruned_bytes).scan(tail_pruned, false); });
    const auto zero = with_budget(0);
    const Predicate missing{1, Op::Between, int64_t(99), int64_t(100)};
    check(zero.scan_all({}).matched == plain.size(), "zero budget permits metadata-only count");
    check(zero.scan(missing).matched == 0, "zero budget permits fully pruned count");
    check(zero.sum_int64(1, {missing}).sum == 0, "zero budget permits fully pruned sum");
    budget_rejects([&] { zero.scan(missing, false); });
    uint64_t first_column = 0;
    for (const auto& g : bounded.groups()) first_column += g[1].bytes;
    boundary(first_column, [&](const Reader& r) {
        check(r.scan_all({missing, filters[1]}, false).matched == 0,
              "empty selection avoids charging later predicate chunks");
        check(r.sum_int64(0, {missing, filters[1]}, false).sum == 0,
              "empty selection avoids charging projection");
    });
    throws([&] { zero.scan_all({missing, {99, Op::Equal, lo, lo}}); },
           "validate predicates before budget/pruning");
    throws([&] { zero.sum_int64(4); }, "validate projection before budget");
    auto expect_limit = [&](ReadLimits cap, const std::string& message) {
        bool rejected = false;
        try { Reader r(path, cap); }
        catch (const std::runtime_error& e) { rejected = e.what() == message; }
        check(rejected, "expected resource rejection: " + message);
    };
    expect_limit({limits.max_footer_bytes - 1, chunk_limit, 256}, "footer byte limit exceeded");
    expect_limit({limits.max_footer_bytes, chunk_limit - 1, 256}, "chunk byte limit exceeded");
    expect_limit({limits.max_footer_bytes, chunk_limit, 255}, "group row limit exceeded");
    expect_limit({0, UINT64_MAX, UINT32_MAX}, "footer byte limit exceeded");
    expect_limit({UINT64_MAX, 0, UINT32_MAX}, "chunk byte limit exceeded");
    expect_limit({UINT64_MAX, UINT64_MAX, 0}, "group row limit exceeded");

    // The cap applies to actual group rows, including short final and empty groups.
    write_file(path, {{"v", Type::Int64}}, {std::vector<int64_t>{7}}, {UINT32_MAX, 1});
    Reader short_group(path, {1024, 8, 1});
    check(short_group.sum_int64(0).sum == 7, "large target size with bounded actual rows");
    write_file(path, {{"v", Type::Int64}}, {std::vector<int64_t>{}}, {UINT32_MAX, 1});
    Reader empty(path, {1024, 0, 0});
    check(empty.scan_all({}).matched == 0, "zero row/chunk limits allow empty tables");

    Reader empty_budget(path, {1024, 0, 0, 0, 0});
    check(empty_budget.sum_int64(0).sum == 0, "empty sum needs no payload budget");
    check(empty_budget.scan({0, Op::Equal, int64_t(0), int64_t(0)}).matched == 0,
          "empty predicate scan needs no payload budget");

    auto save = [&](const std::string& bytes) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), std::streamsize(bytes.size()));
        if (!out) throw std::runtime_error("fixture write failed");
    };
    // Tiny, structurally valid chunks can advertise billions of decoded rows.
    std::string packed_zero(9, char(0)), huge_run;
    append_le(huge_run, UINT32_MAX, 4);
    append_le(huge_run, 0, 8);
    for (const auto& fixture : {dimension_fixture(UINT32_MAX, Encoding::BitPacked, packed_zero),
                                dimension_fixture(UINT32_MAX, Encoding::RLE, huge_run)}) {
        save(fixture);
        check(Reader(path).rows() == UINT32_MAX, "default permits v1 compressed dimensions");
        expect_limit({1024, 1024, 64}, "group row limit exceeded");
    }
    auto malformed_packed = packed_zero;
    malformed_packed[8] = 65;
    save(dimension_fixture(1, Encoding::BitPacked, malformed_packed));
    const auto no_payload = with_budget(0);
    budget_rejects([&] { no_payload.read_column(0, 0); });
    budget_rejects([&] { no_payload.scan({0, Op::Equal, int64_t(0), int64_t(0)}, false); });
    budget_rejects([&] { no_payload.sum_int64(0); });
    throws([&] { Reader(path).read_column(0, 0); }, "permitted malformed payload still rejected");
    const auto valid = dimension_fixture(1, Encoding::BitPacked, packed_zero);
    save(valid);
    check(Reader(path, {1024, 9, 1}).read_column(0, 0) == Column(std::vector<int64_t>{0}),
          "independently built zero-width packed fixture");
    for (size_t size = 0; size < valid.size(); ++size) {
        save(valid.substr(0, size));
        throws([&] { Reader r(path, {1024, 1024, 64}); }, "every truncated prefix is rejected");
    }
    // Footer dimensions and lengths must fail without trusting attacker allocations.
    auto overwrite = [](std::string& bytes, size_t at, uint64_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i) bytes.at(at + i) = char(value >> (8 * i));
    };
    const size_t footer = 16 + packed_zero.size();
    for (const auto& mutation : std::vector<std::pair<size_t, unsigned>>{
             {valid.size() - 16, 8}, {footer, 8}, {footer + 8, 4}, {footer + 12, 4},
             {footer + 16, 4}, {footer + 22 + 13, 8}}) {
        auto broken = valid;
        overwrite(broken, mutation.first, UINT64_MAX, mutation.second);
        save(broken);
        throws([&] { Reader r(path, {1024, 1024, 64}); }, "oversized footer dimension rejected");
    }
}
void predicate_limit_tests(const Scratch& scratch) {
    const auto path = scratch.file("predicate-limit.mc");
    auto limited = [&](uint64_t count) {
        ReadLimits cap;
        cap.max_predicates_per_operation = count;
        return Reader(path, cap);
    };
    auto rejects_with = [&](const std::function<void()>& operation, const std::string& message) {
        bool rejected = false;
        try { operation(); }
        catch (const std::runtime_error& e) { rejected = e.what() == message; }
        check(rejected, "predicate limit error: " + message);
    };
    const std::string limit_error = "operation predicate limit exceeded";
    for (size_t rows : {size_t(0), size_t(2051)}) {
        std::vector<int64_t> plain(rows), packed(rows), runs(rows);
        std::vector<std::string> text(rows), dict(rows), text_runs(rows);
        for (size_t i = 0; i < rows; ++i) {
            plain[i] = i % 2 ? INT64_MAX : INT64_MIN;
            packed[i] = int64_t(i % 31) - 15;
            runs[i] = (i / 256) % 2 ? INT64_MAX : INT64_MIN;
            text[i] = "unique-" + std::to_string(i);
            dict[i] = std::string("dictionary\0entry-", 17) + std::to_string(i % 3);
            text_runs[i] = std::string(40, char('a' + i / 256));
        }
        const std::vector<Column> columns{plain, packed, runs, text, dict, text_runs};
        write_file(path, {{"plain", Type::Int64}, {"packed", Type::Int64}, {"runs", Type::Int64},
                          {"text", Type::String}, {"dict", Type::String}, {"text_runs", Type::String}},
                   columns, {1024, 1024});
        const std::vector<Predicate> filters{
            {0, Op::Equal, int64_t(INT64_MAX), std::string("ignored")},
            {1, Op::Between, int64_t(-8), int64_t(9)},
            {2, Op::Between, int64_t(INT64_MIN), int64_t(INT64_MAX)},
            {3, Op::Between, std::string("unique-1"), std::string("unique-9")},
            {4, Op::Equal, std::string("dictionary\0entry-1", 18), std::string()},
            {5, Op::Between, std::string(40, 'a'), std::string(40, 'z')}};
        if (rows) {
            const auto reader = limited(6);
            const std::vector<Encoding> codecs{Encoding::Plain, Encoding::BitPacked, Encoding::RLE,
                                               Encoding::Plain, Encoding::Dictionary, Encoding::RLE};
            for (size_t c = 0; c < codecs.size(); ++c)
                check(reader.groups()[0][c].encoding == codecs[c], "predicate-limit fixture codec");
        }
        scans(limited(1), columns, filters);
        for (size_t n = 0; n <= filters.size(); ++n) {
            const std::vector<Predicate> query(filters.begin(), filters.begin() + n);
            const auto exact = limited(n);
            conjunctions(exact, columns, query);
            if (n) {
                const auto below = limited(n - 1);
                for (bool zones : {false, true}) {
                    rejects_with([&] { below.scan_all(query, zones); }, limit_error);
                    rejects_with([&] { below.sum_int64(0, query, zones); }, limit_error);
                }
                // Rejections do not retain operation state.
                conjunctions(below, columns, {});
            }
        }
        const std::vector<Predicate> duplicates(64, filters[1]);
        conjunctions(limited(64), columns, duplicates);
        const auto below = limited(63);
        rejects_with([&] { below.scan_all(duplicates); }, limit_error);
        rejects_with([&] { below.sum_int64(1, duplicates); }, limit_error);
        // Existing seven-field initializers and defaults retain unlimited counts.
        conjunctions(Reader(path), columns, duplicates);
        conjunctions(Reader(path, {UINT64_MAX, UINT64_MAX, UINT32_MAX, UINT64_MAX,
                                  UINT64_MAX, UINT64_MAX, UINT64_MAX}), columns, duplicates);
        const auto zero = limited(0);
        for (const auto& p : filters) {
            rejects_with([&] { zero.scan(p); }, limit_error);
            rejects_with([&] { zero.scan_all({p}); }, limit_error);
            rejects_with([&] { zero.sum_int64(0, {p}); }, limit_error);
        }
        const Predicate missing{1, Op::Equal, int64_t(100), int64_t(0)};
        rejects_with([&] { zero.scan_all({missing}); }, limit_error);
        const Predicate invalid{99, Op::Equal, int64_t(0), int64_t(0)};
        // Count check precedes predicate validation, even on empty/pruned tables.
        rejects_with([&] { zero.scan_all({invalid}); }, limit_error);
        rejects_with([&] { zero.sum_int64(0, {invalid}); }, limit_error);
        rejects_with([&] { limited(1).scan_all({missing, invalid}); }, limit_error);
        throws([&] { limited(2).scan_all({missing, invalid}); },
               "within-cap later predicates validated before pruning");
        throws([&] { limited(1).scan(invalid); }, "accepted count still validates columns");
        throws([&] { limited(1).scan({0, Op(99), int64_t(0), int64_t(0)}); },
               "accepted count still validates operations");
        rejects_with([&] { limited(1).scan({0, Op::Equal, std::string(), int64_t(0)}); },
                     "predicate type mismatch");
        rejects_with([&] { zero.sum_int64(3, {invalid}); }, "sum requires integer column");
        // Projection validation retains precedence over the predicate count cap.
        bool bad_projection = false;
        try { zero.sum_int64(99, {invalid}); }
        catch (const std::out_of_range&) { bad_projection = true; }
        check(bad_projection, "projection column validated before predicate limit");
        conjunctions(zero, columns, {});
        if (rows) {
            check(zero.read_column(0, 1) == Column(std::vector<int64_t>(packed.begin(), packed.begin() + 1024)),
                  "zero predicate limit does not restrict materialization");
        }
    }
}
void predicate_evaluation_budget_tests(const Scratch& scratch) {
    const auto path = scratch.file("predicate-evaluations.mc");
    auto limited = [&](uint64_t budget) {
        ReadLimits limits;
        limits.max_predicate_evaluations_per_operation = budget;
        return Reader(path, limits);
    };
    auto rejects_with = [&](const std::function<void()>& operation, const std::string& error) {
        bool caught = false;
        try { operation(); }
        catch (const std::runtime_error& e) { caught = e.what() == error; }
        check(caught, "evaluation budget error: " + error);
    };
    const std::string exhausted = "operation predicate evaluation limit exceeded";
    const size_t n = 3077, group_size = 1025;
    std::vector<int64_t> plain(n), packed(n), runs(n);
    std::vector<std::string> text(n), dict(n), text_runs(n);
    for (size_t i = 0; i < n; ++i) {
        plain[i] = i % 2 ? INT64_MAX : INT64_MIN;
        packed[i] = int64_t(i % 31) - 15;
        runs[i] = (i / 256) % 2 ? INT64_MAX : INT64_MIN;
        text[i] = "unique-" + std::to_string(i) + std::string("\0tail", 5);
        dict[i] = std::string("dictionary\0entry-", 17) + std::to_string(i % 3);
        text_runs[i] = std::string(40, char('a' + i / 256));
    }
    const std::vector<Column> columns{plain, packed, runs, text, dict, text_runs};
    write_file(path, {{"plain", Type::Int64}, {"packed", Type::Int64}, {"runs", Type::Int64},
                      {"text", Type::String}, {"dict", Type::String}, {"text_runs", Type::String}},
               columns, {group_size, group_size});
    const Reader source(path);
    const std::vector<Encoding> codecs{Encoding::Plain, Encoding::BitPacked, Encoding::RLE,
                                       Encoding::Plain, Encoding::Dictionary, Encoding::RLE};
    std::vector<Predicate> all;
    std::vector<uint64_t> units(columns.size(), 0);
    // Count expected evaluation sites from the source values, never a payload decoder.
    for (size_t c = 0; c < columns.size(); ++c) {
        check(source.groups()[0][c].encoding == codecs[c], "evaluation fixture codec");
        all.push_back(std::visit([&](const auto& values) {
            const auto bounds = std::minmax_element(values.begin(), values.end());
            return Predicate{c, Op::Between, Value(*bounds.first), Value(*bounds.second)};
        }, columns[c]));
        for (size_t g = 0; g < source.groups().size(); ++g) {
            std::visit([&](const auto& values) {
                const auto begin = values.begin() + g * group_size;
                const auto end = begin + source.groups()[g][c].rows;
                const auto codec = source.groups()[g][c].encoding;
                if (codec == Encoding::RLE) {
                    ++units[c];
                    for (auto it = begin + 1; it != end; ++it) units[c] += *it != *(it - 1);
                } else if (codec == Encoding::Dictionary) {
                    auto distinct = std::vector<typename std::decay_t<decltype(values)>::value_type>(begin, end);
                    std::sort(distinct.begin(), distinct.end());
                    units[c] += std::unique(distinct.begin(), distinct.end()) - distinct.begin();
                } else units[c] += end - begin;
            }, columns[c]);
        }
        for (size_t duplicates : {size_t(1), size_t(3)}) for (bool zones : {false, true}) {
            const std::vector<Predicate> query(duplicates, all.back());
            const auto budget = duplicates * (units[c] + (zones ? source.groups().size() : 0));
            const auto exact = limited(budget);
            for (int repeat = 0; repeat < 2; ++repeat) {
                const auto result = exact.scan_all(query, zones);
                check(result.matched == conjunction_oracle(columns, query), "exact evaluation count oracle");
                const auto sum = exact.sum_int64(0, query, zones);
                check(sum.scan.matched == result.matched && uint64_t(sum.sum) == sum_oracle(columns, 0, query),
                      "exact evaluation sum oracle, including shared projection");
            }
            if (duplicates == 1)
                check(exact.scan(query.front(), zones).matched == oracle(columns[c], query.front()),
                      "single scan evaluation budget");
            const auto below = limited(budget - 1);
            rejects_with([&] { below.scan_all(query, zones); }, exhausted);
            rejects_with([&] { below.sum_int64(0, query, zones); }, exhausted);
            check(below.scan_all({}).matched == n, "evaluation state resets after failure");
        }
    }
    for (bool zones : {false, true}) {
        uint64_t budget = zones ? all.size() * source.groups().size() : 0;
        for (auto count : units) budget += count;
        const auto exact = limited(budget);
        for (int order = 0; order < 2; ++order) {
            auto query = all;
            if (order) std::reverse(query.begin(), query.end());
            check(exact.scan_all(query, zones).matched == conjunction_oracle(columns, query),
                  "multi-column evaluation count oracle");
            for (size_t projection : {size_t(0), size_t(1), size_t(2)}) {
                const auto result = exact.sum_int64(projection, query, zones);
                check(result.scan.matched == conjunction_oracle(columns, query) &&
                      uint64_t(result.sum) == sum_oracle(columns, projection, query),
                      "multi-column evaluation sum oracle");
            }
        }
        rejects_with([&] { limited(budget - 1).scan_all(all, zones); }, exhausted);
        rejects_with([&] { limited(budget - 1).sum_int64(1, all, zones); }, exhausted);
    }
    for (size_t c = 0; c < columns.size(); ++c) for (bool zones : {false, true}) {
        const Predicate reversed{c, Op::Between, all[c].high, all[c].low};
        const std::vector<Predicate> query{reversed, all[c], all[(c + 1) % columns.size()]};
        const uint64_t budget = zones ? source.groups().size() : units[c];
        const auto exact = limited(budget);
        check(exact.scan_all(query, zones).matched == conjunction_oracle(columns, query),
              "every codec short-circuits later predicates");
        check(uint64_t(exact.sum_int64(0, query, zones).sum) == sum_oracle(columns, 0, query),
              "every codec short-circuits sum predicates");
        rejects_with([&] { limited(budget - 1).scan_all(query, zones); }, exhausted);
    }
    const Predicate absent{1, Op::Equal, int64_t(999), std::string("ignored")};
    for (bool zones : {false, true}) {
        // Failure on the first predicate skips the remaining checks and later columns.
        const uint64_t budget = zones ? source.groups().size() : units[1];
        const std::vector<Predicate> query{absent, all[1], all[4]};
        const auto exact = limited(budget);
        check(exact.scan_all(query, zones).matched == conjunction_oracle(columns, query),
              "short-circuit evaluation count oracle");
        check(uint64_t(exact.sum_int64(0, query, zones).sum) == sum_oracle(columns, 0, query),
              "short-circuit evaluation sum oracle");
        rejects_with([&] { limited(budget - 1).scan_all(query, zones); }, exhausted);
    }
    const Predicate maximum{0, Op::Equal, int64_t(INT64_MAX), std::string()};
    for (bool zones : {false, true}) for (bool reversed : {false, true}) {
        std::vector<Predicate> query{maximum, all[0]};
        if (reversed) std::reverse(query.begin(), query.end());
        const uint64_t budget = n + (reversed ? n : oracle(columns[0], maximum)) +
                                (zones ? 2 * source.groups().size() : 0);
        const auto exact = limited(budget);
        check(exact.scan_all(query, zones).matched == conjunction_oracle(columns, query),
              "selective predicates charge only reached checks, in either order");
        check(uint64_t(exact.sum_int64(0, query, zones).sum) == sum_oracle(columns, 0, query),
              "selective shared projection budget and wrapping sum oracle");
        rejects_with([&] { limited(budget - 1).scan_all(query, zones); }, exhausted);
    }
    const auto zero = limited(0);
    conjunctions(zero, columns, {});
    check(zero.read_column(0, 1) == Column(std::vector<int64_t>(packed.begin(), packed.begin() + group_size)),
          "materialization uses no predicate evaluations");
    rejects_with([&] { zero.scan(absent); }, exhausted);
    rejects_with([&] { zero.scan({0, Op::Equal, std::string(), int64_t(0)}); }, "predicate type mismatch");
    rejects_with([&] { zero.scan_all({absent, {1, Op(99), int64_t(0), int64_t(0)}}); },
                 "invalid predicate operation");
    rejects_with([&] { zero.sum_int64(3, {absent}); }, "sum requires integer column");
    // A finite budget must not hide malformed loaded payloads when it suffices.
    { std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
      file.seekp(std::streamoff(source.groups()[0][1].offset + 8)); file.put(char(65)); }
    rejects_with([&] { limited(UINT64_MAX).scan(all[1], false); }, "invalid packed data");
    rejects_with([&] { limited(units[1]).scan(all[1], false); }, "invalid packed data");
    // Empty tables evaluate nothing, even with supplied predicates and zone maps.
    write_file(path, {{"v", Type::Int64}}, {std::vector<int64_t>{}});
    conjunctions(limited(0), {std::vector<int64_t>{}}, {{0, Op::Equal, int64_t(1), int64_t(0)}});
}
void metadata_entry_limit_tests(const Scratch& scratch) {
    const auto path = scratch.file("metadata-limit.mc");
    auto limits = [](uint64_t entries) {
        ReadLimits cap;
        cap.max_metadata_entries = entries;
        return cap;
    };
    auto rejects_with = [&](uint64_t entries, const std::string& message) {
        bool rejected = false;
        try { Reader reader(path, limits(entries)); }
        catch (const std::runtime_error& e) { rejected = e.what() == message; }
        check(rejected, "metadata limit error: " + message);
    };
    for (size_t width : {size_t(1), size_t(7), size_t(64)}) {
        for (size_t rows : {size_t(0), size_t(1), size_t(17)}) {
            for (uint32_t group_size : {1u, 8u}) {
                std::vector<Field> schema;
                std::vector<Column> columns;
                for (size_t c = 0; c < width; ++c) {
                    schema.push_back({"column-" + std::to_string(c), c % 2 ? Type::String : Type::Int64});
                    if (c % 2) {
                        std::vector<std::string> values;
                        for (size_t r = 0; r < rows; ++r)
                            values.push_back(std::string("a\0", 2) + std::to_string(r % 3));
                        columns.push_back(values);
                    } else {
                        std::vector<int64_t> values;
                        for (size_t r = 0; r < rows; ++r)
                            values.push_back(r % 2 ? INT64_MAX : -int64_t(r));
                        columns.push_back(values);
                    }
                }
                write_file(path, schema, columns, {group_size, 8});
                const uint64_t groups = (rows + group_size - 1) / group_size;
                const uint64_t entries = width + groups * width;
                Reader exact(path, limits(entries));
                check(exact.schema().size() == width && exact.groups().size() == groups,
                      "exact metadata limit accepts schema and groups");
                for (size_t g = 0; g < groups; ++g) {
                    const auto begin = g * group_size, end = std::min(rows, begin + group_size);
                    for (size_t c = 0; c < width; ++c) {
                        const auto wanted = std::visit([&](const auto& values) -> Column {
                            using Vec = std::decay_t<decltype(values)>;
                            return Vec(values.begin() + begin, values.begin() + end);
                        }, columns[c]);
                        check(exact.read_column(g, c) == wanted, "metadata-limited source roundtrip");
                    }
                }
                const std::vector<Predicate> filters{{0, Op::Between, int64_t(-8), int64_t(0)}};
                scans(exact, columns, filters);
                conjunctions(exact, columns, filters);
                if (width > 1) {
                    auto both = filters;
                    both.push_back({1, Op::Equal, std::string("a\0", 2) + "0", std::string()});
                    conjunctions(exact, columns, both);
                }
                check(uint64_t(exact.sum_int64(0).sum) == sum_oracle(columns, 0, {}),
                      "metadata-limited unfiltered sum matches scalar oracle");
                rejects_with(entries - 1, "metadata entry limit exceeded");
                rejects_with(0, "metadata entry limit exceeded");
                check(Reader(path).rows() == rows, "default metadata limit remains unlimited");
                // Limits are per construction; a rejected Reader does not affect later opens.
                check(Reader(path, limits(entries)).scan_all({}).matched == rows,
                      "metadata limit exact reopen after rejection");
            }
        }
    }
    // Independently construct a tiny footer advertising billions of groups.
    // Keep its dimensions plausible, but make the first schema name truncated:
    // the entry cap must reject before parsing names or allocating group objects.
    for (uint32_t width : {1u, 3u}) {
        std::string bytes = "MINICOL1";
        append_le(bytes, 1, 4);
        append_le(bytes, 1, 4); // one row per group
        std::string footer;
        append_le(footer, UINT32_MAX, 8);
        append_le(footer, width, 4);
        append_le(footer, UINT32_MAX, 4);
        append_le(footer, UINT32_MAX, 4); // impossible name length
        footer.resize(16 + width * 5, '\0');
        bytes += footer;
        append_le(bytes, footer.size(), 8);
        bytes += "MCOLEND1";
        { std::ofstream out(path, std::ios::binary | std::ios::trunc);
          out.write(bytes.data(), std::streamsize(bytes.size())); }
        const uint64_t entries = uint64_t(width) * (uint64_t(UINT32_MAX) + 1);
        rejects_with(entries - 1, "metadata entry limit exceeded");
        rejects_with(entries, "truncated string");
    }
}
void decoded_row_budget_tests(const Scratch& scratch) {
    const auto path = scratch.file("row-budget.mc");
    const int64_t lo = INT64_MIN, hi = INT64_MAX;
    constexpr size_t n = 2051, group_size = 1025;
    std::vector<int64_t> plain(n), packed(n), runs(n);
    std::vector<std::string> text(n), dict(n), text_runs(n);
    for (size_t i = 0; i < n; ++i) {
        plain[i] = i % 2 ? hi : lo;
        packed[i] = int64_t(i % 31);
        runs[i] = (i / 512) % 2 ? hi : lo;
        text[i] = "unique-" + std::to_string(100000 + i);
        dict[i] = "long-dictionary-value-" + std::to_string(i % 3);
        text_runs[i] = std::string(40, char('a' + i / 512));
    }
    const std::vector<Column> columns{plain, packed, runs, text, dict, text_runs};
    write_file(path, {{"plain", Type::Int64}, {"packed", Type::Int64}, {"runs", Type::Int64},
                      {"text", Type::String}, {"dict", Type::String}, {"text_runs", Type::String}},
               columns, {group_size, 256});
    auto with_budget = [&](uint64_t rows) {
        return Reader(path, {UINT64_MAX, UINT64_MAX, UINT32_MAX, UINT64_MAX, UINT64_MAX, rows});
    };
    auto rejects = [&](const std::function<void()>& operation) {
        bool rejected = false;
        try { operation(); }
        catch (const std::runtime_error& e) {
            rejected = std::string(e.what()) == "operation decoded row limit exceeded";
        }
        check(rejected, "operation rejects with decoded row budget error");
    };
    auto boundary = [&](uint64_t rows, const std::function<void(const Reader&)>& operation) {
        const auto exact = with_budget(rows);
        operation(exact);
        operation(exact);
        const auto below = with_budget(rows - 1);
        rejects([&] { operation(below); });
        // Failure must not retain a partially consumed budget on the Reader.
        check(below.scan_all({}).matched == n, "row budget failure permits metadata count");
        check(below.read_column(2, 0) == Column(std::vector<int64_t>{plain.back()}),
              "row budget resets after failed operation");
    };
    const std::vector<Encoding> encodings{Encoding::Plain, Encoding::BitPacked, Encoding::RLE,
                                          Encoding::Plain, Encoding::Dictionary, Encoding::RLE};
    const Reader unlimited(path);
    for (size_t c = 0; c < columns.size(); ++c) {
        check(unlimited.groups()[0][c].encoding == encodings[c], "row budget fixture codec");
        for (size_t g = 0; g < unlimited.groups().size(); ++g) {
            const size_t begin = g * group_size, end = std::min(n, begin + group_size);
            const Column wanted = std::visit([&](const auto& v) -> Column {
                using Vec = std::decay_t<decltype(v)>;
                return Vec(v.begin() + begin, v.begin() + end);
            }, columns[c]);
            // A one-row tail uses a direct check: below-boundary budget is zero.
            const auto exact = with_budget(end - begin);
            check(exact.read_column(g, c) == wanted, "row budget source roundtrip");
            rejects([&] { with_budget(end - begin - 1).read_column(g, c); });
        }
        const Value low = c < 3 ? Value(lo) : Value(std::string());
        const Value high = c < 3 ? Value(hi) : Value(std::string(1, char(127)));
        const Predicate all{c, Op::Between, low, high};
        for (bool zones : {false, true}) {
            boundary(n, [&](const Reader& r) {
                check(r.scan(all, zones).matched == oracle(columns[c], all), "row budget scalar count");
                check(r.scan_all({all, all}, zones).matched == oracle(columns[c], all),
                      "repeated predicates decode each chunk once");
            });
            for (size_t projection : {size_t(0), size_t(1), size_t(2)}) {
                boundary(2 * n, [&](const Reader& r) {
                    const auto result = r.sum_int64(projection, {all}, zones);
                    check(uint64_t(result.sum) == sum_oracle(columns, projection, {all}) &&
                          result.scan.matched == conjunction_oracle(columns, {all}),
                          "sum charges projection pass including retained chunks");
                    uint64_t bytes = r.metadata_bytes();
                    for (const auto& group : r.groups()) {
                        bytes += group[c].bytes;
                        if (c != projection) bytes += group[projection].bytes;
                    }
                    check(result.scan.bytes_read == bytes, "row charging preserves payload byte reuse");
                });
            }
        }
    }
    const std::vector<Predicate> filters{{1, Op::Between, int64_t(0), int64_t(30)},
                                         {4, Op::Equal, dict[1], std::string()}};
    boundary(2 * n, [&](const Reader& r) {
        check(r.scan_all(filters, false).matched == conjunction_oracle(columns, filters),
              "row budget accumulates across AND columns and groups");
    });
    boundary(3 * n, [&](const Reader& r) {
        check(uint64_t(r.sum_int64(0, filters, false).sum) == sum_oracle(columns, 0, filters),
              "row budget accumulates across AND and projected sum");
    });
    boundary(n, [&](const Reader& r) {
        check(uint64_t(r.sum_int64(0).sum) == sum_oracle(columns, 0, {}),
              "unfiltered sum charges one pass per group");
    });
    const Predicate absent{1, Op::Equal, int64_t(99), int64_t(0)};
    const auto zero = with_budget(0);
    check(zero.scan_all({}).matched == n, "zero row budget permits metadata count");
    check(zero.scan(absent).matched == 0 && zero.sum_int64(0, {absent}).sum == 0,
          "zero row budget permits full zone pruning");
    rejects([&] { zero.scan(absent, false); });
    boundary(n, [&](const Reader& r) {
        check(r.scan_all({absent, filters[1]}, false).matched == 0 &&
              r.sum_int64(0, {absent, filters[1]}, false).sum == 0,
              "empty selection avoids later decoding charges");
    });
    const Predicate last_group{3, Op::Equal, text.back(), std::string()};
    check(with_budget(1).scan(last_group).matched == oracle(columns[3], last_group),
          "partial pruning charges only surviving group rows");
    throws([&] { zero.scan_all({absent, {99, Op::Equal, lo, lo}}); },
           "invalid predicate checked before row budget");
    throws([&] { zero.sum_int64(3); }, "invalid projection checked before row budget");

    // Tiny compressed inputs must fail before expansion or payload validation.
    for (Encoding codec : {Encoding::RLE, Encoding::BitPacked}) {
        std::string payload;
        if (codec == Encoding::RLE) {
            append_le(payload, UINT32_MAX, 4);
            append_le(payload, 0, 8);
        } else payload.assign(9, char(0));
        const auto fixture = dimension_fixture(UINT32_MAX, codec, payload);
        { std::ofstream out(path, std::ios::binary); out.write(fixture.data(), std::streamsize(fixture.size())); }
        const auto bounded = with_budget(64);
        rejects([&] { bounded.read_column(0, 0); });
        rejects([&] { bounded.scan({0, Op::Equal, int64_t(0), int64_t(0)}, false); });
        rejects([&] { bounded.sum_int64(0); });
        if (codec == Encoding::RLE)
            check(with_budget(UINT32_MAX).scan({0, Op::Equal, int64_t(0), int64_t(0)}).matched == UINT32_MAX,
                  "encoded RLE count charges logical rows at exact 64-bit boundary");
    }
    auto malformed = dimension_fixture(1, Encoding::BitPacked, std::string(9, char(0)));
    malformed[24] = 65;
    { std::ofstream out(path, std::ios::binary); out.write(malformed.data(), std::streamsize(malformed.size())); }
    rejects([&] { with_budget(0).read_column(0, 0); });
    throws([&] { with_budget(1).read_column(0, 0); }, "permitted malformed chunk still rejected");
    write_file(path, {{"empty", Type::Int64}}, {std::vector<int64_t>{}});
    check(with_budget(0).scan_all({}).matched == 0 && with_budget(0).sum_int64(0).sum == 0,
          "empty table consumes no decoded rows");
}
void decoded_string_budget_tests(const Scratch& scratch) {
    const auto path = scratch.file("string-budget.mc");
    for (Encoding codec : {Encoding::Plain, Encoding::Dictionary, Encoding::RLE}) {
        std::vector<std::string> text(2051);
        std::vector<int64_t> numbers(text.size(), 7);
        for (size_t i = 0; i < text.size(); ++i) {
            if (codec == Encoding::Plain) text[i] = "unique-" + std::to_string(i);
            else if (codec == Encoding::Dictionary)
                text[i] = std::string("long-label\0", 11) + std::string((i % 3) * 7, 'x');
            else text[i] = std::string(40, char('a' + i / 512));
        }
        const std::vector<Column> columns{numbers, text};
        write_file(path, {{"i", Type::Int64}, {"s", Type::String}}, columns, {1025, 1025});
        Reader original(path);
        uint64_t largest = 0;
        for (size_t g = 0; g < original.groups().size(); ++g) {
            if (g == 0) check(original.groups()[g][1].encoding == codec, "string budget fixture codec");
            const size_t begin = g * 1025, end = std::min(begin + 1025, text.size());
            uint64_t bytes = 0;
            for (size_t i = begin; i < end; ++i) bytes += text[i].size();
            largest = std::max(largest, bytes);
            Reader exact(path, {UINT64_MAX, UINT64_MAX, UINT32_MAX, bytes});
            check(exact.read_column(g, 1) == Column(std::vector<std::string>(
                      text.begin() + begin, text.begin() + end)), "exact string budget roundtrip");
            Reader below(path, {UINT64_MAX, UINT64_MAX, UINT32_MAX, bytes - 1});
            bool rejected = false;
            try { below.read_column(g, 1); }
            catch (const std::runtime_error& e) {
                rejected = std::string(e.what()) == "decoded string byte limit exceeded";
            }
            check(rejected, "one byte below decoded string total rejects");
        }
        Reader exact(path, {UINT64_MAX, UINT64_MAX, UINT32_MAX, largest});
        const Predicate text_filter{1, Op::Equal, text[1], std::string()};
        scans(exact, columns, {text_filter});
        conjunctions(exact, columns, {{0, Op::Equal, int64_t(7), int64_t(0)}, text_filter});
        Reader zero(path, {UINT64_MAX, UINT64_MAX, UINT32_MAX, 0});
        for (bool zones : {false, true}) {
            throws([&] { zero.scan(text_filter, zones); }, "encoded scan enforces string budget");
            throws([&] { zero.sum_int64(0, {text_filter}, zones); }, "sum predicate enforces string budget");
            check(zero.scan_all({{0, Op::Equal, int64_t(99), int64_t(0)}, text_filter}, zones).matched == 0,
                  "pruning and empty selections avoid oversized string chunks");
        }
        check(zero.scan_all({}).matched == text.size(), "metadata-only scan avoids string budget");
        check(zero.sum_int64(0).sum == int64_t(7 * text.size()), "string cap does not affect integer chunks");
        throws([&] { exact.scan_all({{99, Op::Equal, int64_t(0), int64_t(0)}, text_filter}); },
               "predicate validation still precedes string budget checks");
    }
    // Independently construct a tiny RLE chunk whose expanded size exceeds 32 bits.
    std::string payload;
    append_le(payload, UINT32_MAX, 4);
    append_le(payload, 3, 4);
    payload += "abc";
    std::string fixture = "MINICOL1";
    append_le(fixture, 1, 4);
    append_le(fixture, UINT32_MAX, 4);
    fixture += payload;
    std::string footer;
    append_le(footer, UINT32_MAX, 8);
    append_le(footer, 1, 4);
    append_le(footer, 1, 4);
    append_le(footer, 1, 4);
    footer += 's';
    footer += char(Type::String);
    append_le(footer, UINT32_MAX, 4);
    footer += char(Encoding::RLE);
    append_le(footer, 16, 8);
    append_le(footer, payload.size(), 8);
    for (int bound = 0; bound < 2; ++bound) {
        append_le(footer, 3, 4);
        footer += "abc";
    }
    fixture += footer;
    append_le(fixture, footer.size(), 8);
    fixture += "MCOLEND1";
    { std::ofstream out(path, std::ios::binary); out.write(fixture.data(), std::streamsize(fixture.size())); }
    const uint64_t expanded = uint64_t(UINT32_MAX) * 3;
    Reader huge_exact(path, {1024, 1024, UINT32_MAX, expanded});
    const Predicate all{0, Op::Equal, std::string("abc"), std::string()};
    check(huge_exact.scan(all).matched == UINT32_MAX, "64-bit expanded byte count without expanding RLE");
    Reader huge_below(path, {1024, 1024, UINT32_MAX, expanded - 1});
    throws([&] { huge_below.read_column(0, 0); }, "huge RLE rejected before materialization");
    throws([&] { huge_below.scan(all, false); }, "64-bit budget one below run expansion rejects");
    // Empty strings consume no decoded bytes; the budget resets for each call.
    for (uint32_t n : {1u, 2049u}) {
        write_file(path, {{"s", Type::String}}, {std::vector<std::string>(n)}, {1025, 1025});
        Reader zero(path, {UINT64_MAX, UINT64_MAX, UINT32_MAX, 0});
        for (int repeat = 0; repeat < 2; ++repeat) {
            check(zero.read_column(0, 0) == Column(std::vector<std::string>(std::min(n, 1025u))),
                  "zero budget accepts empty strings");
            scans(zero, {std::vector<std::string>(n)}, {{0, Op::Equal, std::string(), std::string()}});
        }
    }
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
        conjunction_tests(scratch);
        aggregate_tests(scratch);
        bounded_input_tests(scratch);
        predicate_limit_tests(scratch);
        predicate_evaluation_budget_tests(scratch);
        metadata_entry_limit_tests(scratch);
        decoded_row_budget_tests(scratch);
        decoded_string_budget_tests(scratch);
        invalid_tests(scratch);
        payload_corruption_tests(scratch);
        std::cout << "PASS: " << checks << " checks (encodings, scalar oracle, edge cases, pruning, malformed files)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n';
        return 1;
    }
}
