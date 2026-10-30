#pragma once
#include "minicol/engine.hpp"
#include <algorithm>

// Test-only deterministic inputs shared by release tests and the fuzz harness.
// Query bounds come from source/decoded rows and deterministic boundary probes,
// never persisted metadata or engine scans.
namespace structured_fuzz {
inline std::vector<minicol::Column> columns() {
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
    return {plain, packed, runs, constant, text, dict, text_runs};
}
inline minicol::Value at(const minicol::Column& column, size_t row) {
    return std::visit([&](const auto& values) -> minicol::Value { return values.at(row); }, column);
}
// At most eight probes per nonempty column. Guard arithmetic at signed edges;
// string extensions preserve embedded NULs and exercise prefix ordering.
inline std::vector<minicol::Value> boundary_probes(const minicol::Column& column) {
    using namespace minicol;
    std::vector<Value> probes;
    if (const auto* ints = std::get_if<std::vector<int64_t>>(&column)) {
        if (ints->empty()) return probes;
        const auto bounds = std::minmax_element(ints->begin(), ints->end());
        probes = {int64_t(INT64_MIN), int64_t(INT64_MAX)};
        for (int64_t value : {*bounds.first, (*ints)[ints->size() / 2], *bounds.second}) {
            if (value != INT64_MIN) probes.emplace_back(value - 1);
            if (value != INT64_MAX) probes.emplace_back(value + 1);
        }
    } else {
        const auto& strings = std::get<std::vector<std::string>>(column);
        if (strings.empty()) return probes;
        const auto& pivot = strings[strings.size() / 2];
        const auto& maximum = *std::max_element(strings.begin(), strings.end());
        probes = {std::string(), std::string(1, '\0'), std::string(1, '\x7f'),
                  std::string(1, '\x80'), std::string(1, '\xff'),
                  pivot.substr(0, pivot.size() / 2), pivot + std::string(1, '\0'),
                  maximum + std::string(1, '\0')};
    }
    std::sort(probes.begin(), probes.end());
    probes.erase(std::unique(probes.begin(), probes.end()), probes.end());
    return probes;
}
inline std::vector<std::vector<minicol::Predicate>> queries(
        const std::vector<minicol::Column>& columns) {
    using namespace minicol;
    std::vector<std::vector<Predicate>> result(1); // Empty conjunction.
    if (columns.empty()) return result;
    const auto rows = std::visit([](const auto& v) { return v.size(); }, columns.front());
    if (!rows) return result;
    std::vector<Predicate> equal, ranges, lower, upper;
    for (size_t c = 0; c < columns.size(); ++c) {
        const auto bounds = std::visit([](const auto& v) {
            const auto b = std::minmax_element(v.begin(), v.end());
            return std::make_pair(Value(*b.first), Value(*b.second));
        }, columns[c]);
        const auto pivot = at(columns[c], rows / 2);
        equal.push_back({c, Op::Equal, pivot, pivot});
        ranges.push_back({c, Op::Between, bounds.first, bounds.second});
        lower.push_back({c, Op::Between, bounds.first, pivot});
        upper.push_back({c, Op::Between, pivot, bounds.second});
        for (size_t row : {size_t(0), rows / 2, rows - 1}) {
            const auto value = at(columns[c], row);
            result.push_back({{c, Op::Equal, value, value}});
        }
        result.push_back({ranges.back()});
        result.push_back({lower.back()});
        result.push_back({upper.back()});
        result.push_back({equal.back(), equal.back()});
        if (bounds.first != bounds.second) {
            result.push_back({{c, Op::Between, bounds.second, bounds.first}});
            result.push_back({{c, Op::Equal, bounds.first, bounds.first},
                              {c, Op::Equal, bounds.second, bounds.second}});
        }
    }
    // Each column leads a selective mixed-column query, avoiding the old
    // prefix-only pattern where an early equality masked later columns.
    if (columns.size() > 1) for (size_t c = 0; c < columns.size(); ++c) {
        const size_t next = (c + 1) % columns.size();
        result.push_back({equal[c], ranges[next]});
        result.push_back({ranges[next], equal[c]});
        result.push_back({lower[c], upper[next]});
        result.push_back({upper[next], lower[c]});
    }
    std::vector<Predicate> prefix;
    for (size_t c = 0; c < columns.size(); ++c) {
        prefix.push_back(equal[c]);
        result.push_back(prefix);
        prefix.push_back(ranges[c]);
        result.push_back(prefix);
    }
    result.push_back(equal); // Shared source row guarantees a survivor.
    result.push_back(ranges);
    std::reverse(equal.begin(), equal.end());
    result.push_back(equal);
    std::reverse(equal.begin(), equal.end());
    // Each probe is tested as an equality and as either inclusive range bound.
    // Some ranges intentionally reverse; others include values absent from the
    // file but inside a zone map. Retain all previous source-value queries.
    std::vector<Predicate> near;
    for (size_t c = 0; c < columns.size(); ++c) {
        const auto probes = boundary_probes(columns[c]);
        const auto pivot = at(columns[c], rows / 2);
        for (const auto& probe : probes) {
            result.push_back({{c, Op::Equal, probe, probe}});
            result.push_back({{c, Op::Between, probe, pivot}});
            result.push_back({{c, Op::Between, pivot, probe}});
        }
        // The surrounding probes keep the pivot while testing mixed-column
        // intersections and early termination in both column orders.
        auto low = pivot, high = pivot;
        for (const auto& probe : probes) {
            if (probe < pivot) low = probe;
            if (pivot < probe) { high = probe; break; }
        }
        near.push_back({c, Op::Between, low, high});
    }
    if (columns.size() > 1) for (size_t c = 0; c < columns.size(); ++c) {
        const size_t next = (c + 1) % columns.size();
        result.push_back({near[c], equal[next]});
        result.push_back({equal[next], near[c]});
    }
    return result;
}
} // namespace structured_fuzz
