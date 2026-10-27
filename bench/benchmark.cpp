#include "minicol/engine.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
using namespace minicol;
struct Record { int64_t key, metric, status; char category[32]; char payload[96]; };
static_assert(sizeof(Record)==152, "baseline record layout changed");
static bool match(const Record& r, const Predicate& p) {
 if(p.column==3) { std::string s(r.category); const auto& lo=std::get<std::string>(p.low); return p.op==Op::Equal ? s==lo : s>=lo && s<=std::get<std::string>(p.high); }
 int64_t v=p.column==0?r.key:p.column==1?r.metric:r.status;
 auto lo=std::get<int64_t>(p.low); return p.op==Op::Equal ? v==lo : v>=lo && v<=std::get<int64_t>(p.high);
}
static ScanResult baseline(const std::string& path, const Predicate& p) {
 std::ifstream in(path,std::ios::binary); if(!in) throw std::runtime_error("baseline open failed");
 std::array<Record,1024> batch; ScanResult result;
 while(in.read(reinterpret_cast<char*>(batch.data()),sizeof(batch)) || in.gcount()) {
  auto bytes=in.gcount(); if(bytes%sizeof(Record)) throw std::runtime_error("partial row record");
  result.bytes_read+=bytes; ++result.batches;
  for(size_t i=0;i<static_cast<size_t>(bytes)/sizeof(Record);++i) result.matched+=match(batch[i],p);
 }
 return result;
}
int main(int argc,char** argv) { try {
 const size_t n=argc>1?std::stoull(argv[1]):1000000; const int reps=argc>2?std::stoi(argv[2]):7;
 const std::string dir=argc>3?argv[3]:"build/data";
 if(n<1000 || reps<1) throw std::runtime_error("rows >= 1000 and repetitions >= 1 required");
 std::filesystem::create_directories(dir);
 std::vector<int64_t> keys(n), metrics(n), statuses(n); std::vector<std::string> categories(n), payloads(n);
 std::mt19937_64 rng(42); std::ofstream rowout(dir+"/rows.bin",std::ios::binary); if(!rowout) throw std::runtime_error("baseline output failed");
 for(size_t i=0;i<n;++i) {
  Record r{}; r.key=keys[i]=i; r.metric=metrics[i]=rng()%1024; r.status=statuses[i]=(i/4096)%8;
  categories[i]="category_"+std::to_string(rng()%16); std::memcpy(r.category,categories[i].data(),categories[i].size());
  for(char& c:r.payload) c='A'+rng()%26;
  payloads[i]=std::string(r.payload,sizeof(r.payload)); rowout.write(reinterpret_cast<const char*>(&r),sizeof(r));
 }
 rowout.close(); if(!rowout) throw std::runtime_error("baseline write failed");
 std::vector<Field> schema={{"key",Type::Int64},{"metric",Type::Int64},{"status",Type::Int64},{"category",Type::String},{"payload",Type::String}};
 std::vector<Column> columns; columns.emplace_back(std::move(keys)); columns.emplace_back(std::move(metrics)); columns.emplace_back(std::move(statuses)); columns.emplace_back(std::move(categories)); columns.emplace_back(std::move(payloads));
 write_file(dir+"/columns.mcol",schema,columns);
 Reader initial(dir+"/columns.mcol");
 std::cerr<<"rows="<<n<<" row_store_bytes="<<std::filesystem::file_size(dir+"/rows.bin")<<" column_store_bytes="<<std::filesystem::file_size(dir+"/columns.mcol")<<" metadata_bytes="<<initial.metadata_bytes()<<" seed=42\n";
 std::array<uint64_t,4> enc{}; for(const auto& g:initial.groups()) for(const auto& c:g) ++enc.at(static_cast<size_t>(c.encoding));
 for(size_t i=0;i<enc.size();++i) std::cerr<<encoding_name(static_cast<Encoding>(i))<<"_chunks="<<enc[i]<<"\n";
 struct Case { const char* name; Predicate p; };
 std::vector<Case> cases={
  {"sorted_key_range",{0,Op::Between,int64_t(n/2),int64_t(n/2+n/100-1)}},
  {"small_integer_equality",{1,Op::Equal,int64_t(7),int64_t(7)}},
  {"repetitive_integer_equality",{2,Op::Equal,int64_t(3),int64_t(3)}},
  {"dictionary_equality",{3,Op::Equal,std::string("category_7"),std::string("category_7")}},
  {"absent_dictionary_value",{3,Op::Equal,std::string("zz_missing"),std::string("zz_missing")}}
 };
 std::cout<<"query,mode,trial,rows,matched,elapsed_ms,rows_per_second,bytes_read,groups_read,groups_skipped,batches\n";
 for(const auto& c:cases) {
  const auto expected=baseline(dir+"/rows.bin",c.p).matched;
  // One untimed warmup of every method. All timed scans reopen files and parse metadata.
  (void)baseline(dir+"/rows.bin",c.p); (void)Reader(dir+"/columns.mcol").scan(c.p,false); (void)Reader(dir+"/columns.mcol").scan(c.p,true);
  for(int trial=0;trial<reps;++trial) for(int j=0;j<3;++j) {
   const int mode=(j+trial)%3; // rotate mode order to reduce ordering bias
   auto start=std::chrono::steady_clock::now();
   auto result=mode==0?baseline(dir+"/rows.bin",c.p):Reader(dir+"/columns.mcol").scan(c.p,mode==2);
   const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
   if(result.matched!=expected) throw std::runtime_error("benchmark result mismatch: "+std::string(c.name));
   std::cout<<c.name<<','<<(mode==0?"row_store":mode==1?"column_no_zones":"column_zones")<<','<<trial<<','<<n<<','<<result.matched<<','<<std::fixed<<std::setprecision(6)<<ms<<','<<n/(ms/1000)<<','<<result.bytes_read<<','<<result.groups_read<<','<<result.groups_skipped<<','<<result.batches<<'\n';
  }
 }
 } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } }
