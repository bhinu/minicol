#pragma once
#include <cstdint>
#include <string>
#include <variant>
#include <vector>
namespace minicol {
using Value = std::variant<int64_t, std::string>;
using Column = std::variant<std::vector<int64_t>, std::vector<std::string>>;
enum class Type : uint8_t { Int64=0, String=1 };
enum class Encoding : uint8_t { Plain=0, Dictionary=1, RLE=2, BitPacked=3 };
enum class Op { Equal, Between };
struct Field { std::string name; Type type; };
struct Predicate { size_t column; Op op; Value low; Value high; };
struct ChunkMeta { uint64_t offset=0, bytes=0; uint32_t rows=0; Encoding encoding=Encoding::Plain; Value min=int64_t(0), max=int64_t(0); };
struct ScanResult { uint64_t matched=0, bytes_read=0, groups_read=0, groups_skipped=0, batches=0; };
struct SumResult { ScanResult scan; int64_t sum=0; };
struct WriteOptions { uint32_t row_group_size=65536; uint32_t sample_size=2048; };
// Opt-in input limits; defaults preserve support for all format-v1 dimensions.
struct ReadLimits {
 uint64_t max_footer_bytes=UINT64_MAX;
 uint64_t max_chunk_bytes=UINT64_MAX;
 uint32_t max_group_rows=UINT32_MAX;
};
void write_file(const std::string& path, const std::vector<Field>& schema, const std::vector<Column>& columns, WriteOptions options={});
class Reader {
public:
 explicit Reader(const std::string& path, ReadLimits limits={});
 const std::vector<Field>& schema() const { return schema_; }
 const std::vector<std::vector<ChunkMeta>>& groups() const { return groups_; }
 uint64_t rows() const { return rows_; }
 uint64_t metadata_bytes() const { return metadata_bytes_; }
 Column read_column(size_t group, size_t column) const;
 ScanResult scan(const Predicate& predicate, bool zone_maps=true) const;
 ScanResult scan_all(const std::vector<Predicate>& predicates, bool zone_maps=true) const;
 // Sum selected int64 values modulo 2^64, interpreted as signed two's complement.
 SumResult sum_int64(size_t column, const std::vector<Predicate>& predicates={}, bool zone_maps=true) const;
private:
 SumResult scan_impl(const std::vector<Predicate>& predicates, bool zone_maps, const size_t* projection) const;
 std::string path_;
 std::vector<Field> schema_;
 std::vector<std::vector<ChunkMeta>> groups_;
 uint64_t rows_=0, metadata_bytes_=0;
};
const char* encoding_name(Encoding encoding);
}
