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
        Reader broken(path);
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
        invalid_tests(scratch);
        payload_corruption_tests(scratch);
        std::cout << "PASS: " << checks << " checks (encodings, scalar oracle, edge cases, pruning, malformed files)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n';
        return 1;
    }
}
