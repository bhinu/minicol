#include "minicol/engine.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace minicol { namespace {
using Bytes=std::vector<uint8_t>;
[[noreturn]] void bad(const char* s) { throw std::runtime_error(s); }
void put(Bytes& b,uint64_t v,unsigned n) { for(unsigned i=0;i<n;++i)b.push_back(uint8_t(v>>(i*8))); }
void str(Bytes& b,const std::string& s) { if(s.size()>UINT32_MAX)bad("string too long");put(b,s.size(),4);b.insert(b.end(),s.begin(),s.end()); }
int64_t signed_bits(uint64_t v) { int64_t x;std::memcpy(&x,&v,8);return x; }
struct Cursor {
 const Bytes& b;size_t p=0;
 uint64_t get(unsigned n) { if(n>b.size()-p)bad("truncated data");uint64_t v=0;for(unsigned i=0;i<n;++i)v|=uint64_t(b[p++])<<(8*i);return v; }
 std::string string() { auto n=get(4);if(n>b.size()-p)bad("truncated string");std::string s(reinterpret_cast<const char*>(b.data()+p),size_t(n));p+=size_t(n);return s; }
 void done() { if(p!=b.size())bad("unexpected trailing bytes"); }
};
void value(Bytes& b,const Value& v) { if(auto p=std::get_if<int64_t>(&v))put(b,uint64_t(*p),8);else str(b,std::get<std::string>(v)); }
Value value(Cursor& c,Type t) { if(t==Type::Int64)return signed_bits(c.get(8));return c.string(); }
bool less(const Value& a,const Value& b) { return a.index()==0?std::get<int64_t>(a)<std::get<int64_t>(b):std::get<std::string>(a)<std::get<std::string>(b); }
bool matches(const Value& v,const Predicate& p) { return p.op==Op::Equal?v==p.low:!less(v,p.low)&&!less(p.high,v); }
Bytes load(std::ifstream& in,uint64_t off,uint64_t n) { if(n>SIZE_MAX||n>uint64_t(std::numeric_limits<std::streamsize>::max()))bad("chunk too large");Bytes b(static_cast<size_t>(n));in.clear();in.seekg(static_cast<std::streamoff>(off));if(!in||!in.read(reinterpret_cast<char*>(b.data()),static_cast<std::streamsize>(n)))bad("file read failed");return b; }
void emit(std::ofstream& out,const Bytes& b) { out.write(reinterpret_cast<const char*>(b.data()),static_cast<std::streamsize>(b.size()));if(!out)bad("file write failed"); }
unsigned width(uint64_t v) { unsigned n=0;while(v){++n;v>>=1;}return n; }
void pack(Bytes& b,uint64_t v,unsigned w,uint64_t bit) { unsigned used=0;while(used<w){auto pos=bit+used;unsigned k=std::min<unsigned>(w-used,8-unsigned(pos%8));b[size_t(pos/8)]|=uint8_t((v>>used)&((1u<<k)-1))<<unsigned(pos%8);used+=k;} }
uint64_t unpack(const Bytes& b,size_t start,unsigned w,uint64_t bit) { uint64_t v=0;unsigned used=0;while(used<w){auto pos=bit+used;unsigned k=std::min<unsigned>(w-used,8-unsigned(pos%8));v|=uint64_t((b[start+size_t(pos/8)]>>unsigned(pos%8))&((1u<<k)-1))<<used;used+=k;}return v; }
struct Encoded { Encoding encoding;Bytes bytes; };
Encoded encode(const Column& col,size_t begin,uint32_t n,uint32_t sample,Value& lo,Value& hi) {
 bool ints=col.index()==0;
 auto at=[&](size_t i)->Value { if(ints)return std::get<0>(col)[begin+i];return std::get<1>(col)[begin+i]; };
 lo=hi=at(0);Bytes plain;
 for(uint32_t i=0;i<n;++i){auto v=at(i);if(less(v,lo))lo=v;if(less(hi,v))hi=v;value(plain,v);}
 Encoded best{Encoding::Plain,std::move(plain)};
 uint32_t sn=std::min(sample,n),transitions=0;std::unordered_map<std::string,uint32_t> sampled;
 int64_t sample_min=0,sample_max=0;if(ints)sample_min=sample_max=std::get<0>(col)[begin];
 // Evenly spaced sample estimates cardinality; adjacent pairs estimate run density.
 for(uint32_t i=0;i<sn;++i){size_t j=size_t(uint64_t(i)*n/sn);if(j+1<n&&at(j)!=at(j+1))++transitions;if(!ints)sampled.emplace(std::get<std::string>(at(j)),0);else {auto x=std::get<0>(col)[begin+j];sample_min=std::min(sample_min,x);sample_max=std::max(sample_max,x);}}
 if(uint64_t(transitions)*2<sn){Bytes r;uint32_t count=1;auto prev=at(0);for(uint32_t i=1;i<n;++i){if(at(i)==prev){++count;continue;}put(r,count,4);value(r,prev);prev=at(i);count=1;}put(r,count,4);value(r,prev);if(r.size()<best.bytes.size())best={Encoding::RLE,std::move(r)};}
 if(ints&&width(uint64_t(sample_max)-uint64_t(sample_min))<64){int64_t base=std::get<int64_t>(lo);uint64_t span=uint64_t(std::get<int64_t>(hi))-uint64_t(base);unsigned w=width(span);uint64_t bytes=(uint64_t(n)*w+7)/8;if(bytes+9<best.bytes.size()){Bytes b;put(b,uint64_t(base),8);put(b,w,1);b.resize(9+size_t(bytes),0);for(uint32_t i=0;i<n;++i)pack(b,uint64_t(std::get<0>(col)[begin+i])-uint64_t(base),w,uint64_t(i)*w+72);best={Encoding::BitPacked,std::move(b)};}}
 else if(!ints&&sampled.size()*2<=sn){std::unordered_map<std::string,uint32_t> ids;std::vector<std::string> dict;std::vector<uint32_t> codes;codes.reserve(n);for(uint32_t i=0;i<n;++i){const auto& s=std::get<1>(col)[begin+i];auto it=ids.find(s);if(it==ids.end()){auto id=uint32_t(dict.size());ids.emplace(s,id);dict.push_back(s);codes.push_back(id);}else codes.push_back(it->second);}Bytes b;put(b,dict.size(),4);for(auto& s:dict)str(b,s);for(auto id:codes)put(b,id,4);if(b.size()<best.bytes.size())best={Encoding::Dictionary,std::move(b)};}
 return best;
}
bool matches_all(const Value& v,const std::vector<const Predicate*>& predicates) {
 for(const auto* p:predicates)if(!matches(v,*p))return false;
 return true;
}
// Calls consume in batches; dictionary scans retain strings once per dictionary only.
template<class Consume> void walk(const Bytes& b,Type type,const ChunkMeta& m,Consume consume,const std::vector<const Predicate*>* predicates=nullptr,uint64_t* direct=nullptr,std::vector<uint32_t>* selected=nullptr) {
 Cursor c{b};std::vector<std::string> dict;std::vector<uint8_t> dict_matches;
 int64_t base=0;unsigned bits=0;size_t packed=0;
 if(m.encoding==Encoding::Dictionary){if(type!=Type::String)bad("dictionary requires string type");auto count=c.get(4);if(count>m.rows||count>(b.size()-c.p)/4)bad("invalid dictionary size");dict.reserve(size_t(count));for(uint64_t i=0;i<count;++i)dict.push_back(c.string());if(b.size()-c.p!=uint64_t(m.rows)*4)bad("invalid dictionary codes size");if(predicates){for(auto& s:dict)dict_matches.push_back(matches_all(s,*predicates));}}
 if(m.encoding==Encoding::BitPacked){if(type!=Type::Int64)bad("bitpacking requires integer type");base=signed_bits(c.get(8));bits=unsigned(c.get(1));if(bits>64||b.size()-c.p!=(uint64_t(m.rows)*bits+7)/8)bad("invalid packed data");packed=c.p;}
 if(m.encoding==Encoding::RLE){uint32_t produced=0;while(produced<m.rows){auto count=c.get(4);auto v=value(c,type);if(!count||count>m.rows-produced)bad("invalid run length");if(predicates&&direct){if(matches_all(v,*predicates)){*direct+=count;if(selected)for(uint32_t i=0;i<count;++i)selected->push_back(produced+i);}}else{for(uint64_t i=0;i<count;++i)consume(v);}produced+=uint32_t(count);}c.done();return;}
 std::array<int64_t,1024> iv{};std::array<uint32_t,1024> ids{};std::array<std::string,1024> sv;
 for(uint32_t offset=0;offset<m.rows;){uint32_t n=std::min<uint32_t>(1024,m.rows-offset);
  for(uint32_t i=0;i<n;++i){if(m.encoding==Encoding::Dictionary){ids[i]=uint32_t(c.get(4));if(ids[i]>=dict.size())bad("invalid dictionary ID");}else if(m.encoding==Encoding::BitPacked)iv[i]=signed_bits(uint64_t(base)+unpack(b,packed,bits,uint64_t(offset+i)*bits));else if(type==Type::Int64)iv[i]=signed_bits(c.get(8));else sv[i]=c.string();}
  for(uint32_t i=0;i<n;++i){if(m.encoding==Encoding::Dictionary){if(predicates&&direct){*direct+=dict_matches[ids[i]];if(selected&&dict_matches[ids[i]])selected->push_back(offset+i);}else consume(Value(dict[ids[i]]));}else if(type==Type::Int64)consume(Value(iv[i]));else consume(Value(sv[i]));}offset+=n;
 }
 if(m.encoding!=Encoding::BitPacked)c.done();
}
} // namespace

const char* encoding_name(Encoding e) { switch(e){case Encoding::Plain:return "plain";case Encoding::Dictionary:return "dictionary";case Encoding::RLE:return "rle";case Encoding::BitPacked:return "bitpacked";}return "unknown"; }
void write_file(const std::string& path,const std::vector<Field>& schema,const std::vector<Column>& columns,WriteOptions options) {
 if(schema.empty()||schema.size()!=columns.size()||schema.size()>UINT32_MAX||!options.row_group_size||!options.sample_size)bad("invalid write options/schema");
 size_t rows=std::visit([](auto& v){return v.size();},columns[0]);
 for(size_t i=0;i<columns.size();++i){if(unsigned(schema[i].type)>1||size_t(schema[i].type)!=columns[i].index()||std::visit([](auto& v){return v.size();},columns[i])!=rows)bad("column type/length mismatch");}
 uint64_t ng=(uint64_t(rows)+options.row_group_size-1)/options.row_group_size;if(ng>UINT32_MAX)bad("too many row groups");
 std::ofstream out(path,std::ios::binary|std::ios::trunc);if(!out)bad("cannot create file");Bytes header{'M','I','N','I','C','O','L','1'};put(header,1,4);put(header,options.row_group_size,4);emit(out,header);
 std::vector<std::vector<ChunkMeta>> groups;
 for(size_t begin=0;begin<rows;){uint32_t n=uint32_t(std::min<size_t>(options.row_group_size,rows-begin));std::vector<ChunkMeta> group;for(size_t ci=0;ci<schema.size();++ci){ChunkMeta m;m.rows=n;m.offset=uint64_t(out.tellp());auto e=encode(columns[ci],begin,n,options.sample_size,m.min,m.max);m.encoding=e.encoding;m.bytes=e.bytes.size();emit(out,e.bytes);group.push_back(std::move(m));}groups.push_back(std::move(group));begin+=n;}
 Bytes footer;put(footer,rows,8);put(footer,schema.size(),4);put(footer,groups.size(),4);for(auto& f:schema){str(footer,f.name);put(footer,unsigned(f.type),1);}for(auto& g:groups)for(auto& m:g){put(footer,m.rows,4);put(footer,unsigned(m.encoding),1);put(footer,m.offset,8);put(footer,m.bytes,8);value(footer,m.min);value(footer,m.max);}emit(out,footer);Bytes tail;put(tail,footer.size(),8);const char* magic="MCOLEND1";tail.insert(tail.end(),magic,magic+8);emit(out,tail);out.flush();if(!out)bad("file flush failed");out.close();if(!out)bad("file close failed");
}
Reader::Reader(const std::string& path):path_(path) {
 std::ifstream in(path,std::ios::binary|std::ios::ate);if(!in)bad("cannot open file");auto end=in.tellg();if(end<32)bad("file too short");uint64_t size=uint64_t(end);auto h=load(in,0,16);if(std::memcmp(h.data(),"MINICOL1",8))bad("invalid file magic");Cursor hc{h,8};if(hc.get(4)!=1)bad("unsupported format version");auto group_size=hc.get(4);if(!group_size)bad("invalid row group size");auto t=load(in,size-16,16);if(std::memcmp(t.data()+8,"MCOLEND1",8))bad("invalid trailer");Cursor tc{t};auto fl=tc.get(8);if(fl>size-32||fl<16)bad("invalid footer size");auto b=load(in,size-16-fl,fl);Cursor c{b};rows_=c.get(8);auto nc=c.get(4),ng=c.get(4);if(!nc||nc>(b.size()-c.p)/5||ng!=(rows_/group_size+(rows_%group_size!=0)))bad("invalid schema/group count");for(uint64_t i=0;i<nc;++i){auto name=c.string();auto ty=c.get(1);if(ty>1)bad("invalid column type");schema_.push_back({name,Type(ty)});}if(ng&&(nc>(b.size()-c.p)/ng/29))bad("footer metadata truncated");
 uint64_t expected_offset=16,total_rows=0;
 for(uint64_t g=0;g<ng;++g){std::vector<ChunkMeta> group;uint32_t group_rows=uint32_t(std::min<uint64_t>(group_size,rows_-total_rows));for(uint64_t ci=0;ci<nc;++ci){ChunkMeta m;m.rows=uint32_t(c.get(4));auto enc=c.get(1);if(enc>3)bad("invalid encoding");m.encoding=Encoding(enc);m.offset=c.get(8);m.bytes=c.get(8);m.min=value(c,schema_[size_t(ci)].type);m.max=value(c,schema_[size_t(ci)].type);if(m.rows!=group_rows||!m.rows||less(m.max,m.min)||m.offset!=expected_offset||m.offset>size-16-fl||m.bytes>size-16-fl-m.offset)bad("invalid chunk metadata");if((m.encoding==Encoding::Dictionary&&schema_[size_t(ci)].type!=Type::String)||(m.encoding==Encoding::BitPacked&&schema_[size_t(ci)].type!=Type::Int64))bad("invalid encoding/type combination");expected_offset+=m.bytes;group.push_back(std::move(m));}total_rows+=group_rows;groups_.push_back(std::move(group));}
 c.done();if(expected_offset!=size-16-fl)bad("unexpected payload bytes");metadata_bytes_=32+fl;
}
Column Reader::read_column(size_t group,size_t column) const {
 if(group>=groups_.size()||column>=schema_.size())throw std::out_of_range("group/column");
 auto& m=groups_[group][column];std::ifstream in(path_,std::ios::binary);auto b=load(in,m.offset,m.bytes);Column result;if(schema_[column].type==Type::Int64)result=std::vector<int64_t>{};else result=std::vector<std::string>{};walk(b,schema_[column].type,m,[&](const Value& v){if(v.index()==0)std::get<0>(result).push_back(std::get<0>(v));else std::get<1>(result).push_back(std::get<1>(v));});return result;
}
ScanResult Reader::scan(const Predicate& p,bool zones) const {
 return scan_all({p},zones);
}
ScanResult Reader::scan_all(const std::vector<Predicate>& predicates,bool zones) const {
 // Validate every predicate before pruning or short-circuiting any payload reads.
 std::vector<size_t> columns;
 std::vector<std::vector<const Predicate*>> by_column;
 for(const auto& p:predicates){
  if(p.column>=schema_.size())throw std::out_of_range("predicate column");
  if(p.op!=Op::Equal&&p.op!=Op::Between)bad("invalid predicate operation");
  size_t ty=size_t(schema_[p.column].type);
  if(p.low.index()!=ty||(p.op==Op::Between&&p.high.index()!=ty))bad("predicate type mismatch");
  auto it=std::find(columns.begin(),columns.end(),p.column);
  if(it==columns.end()){columns.push_back(p.column);by_column.push_back({&p});}
  else by_column[size_t(it-columns.begin())].push_back(&p);
 }
 ScanResult result;result.bytes_read=metadata_bytes_;
 if(predicates.empty()){result.matched=rows_;return result;}
 std::ifstream in(path_,std::ios::binary);
 for(const auto& group:groups_){
  bool skip=false;
  if(zones)for(const auto& p:predicates){
   const auto& m=group[p.column];
   skip=p.op==Op::Equal?(less(p.low,m.min)||less(m.max,p.low)):
       (less(p.high,m.min)||less(m.max,p.low)||less(p.high,p.low));
   if(skip)break;
  }
  if(skip){++result.groups_skipped;continue;}
  ++result.groups_read;result.batches+=(uint64_t(group.front().rows)+1023)/1024;
  std::vector<uint32_t> selection;
  for(size_t ci=0;ci<columns.size();++ci){
   const auto column=columns[ci];const auto& m=group[column];
   auto b=load(in,m.offset,m.bytes);result.bytes_read+=m.bytes;
   uint64_t matched=0;uint32_t row=0;
   // Single-column counts keep the encoded count fast path and need no selection.
   if(columns.size()==1){
    walk(b,schema_[column].type,m,[&](const Value& v){matched+=matches_all(v,by_column[ci]);},&by_column[ci],&matched);
    result.matched+=matched;break;
   }
   std::vector<uint32_t> hits;
   walk(b,schema_[column].type,m,[&](const Value& v){
    if(matches_all(v,by_column[ci]))hits.push_back(row);
    ++row;
   },&by_column[ci],&matched,&hits);
   if(ci==0)selection=std::move(hits);
   else {
    // Both vectors are sorted group-local row offsets. Compact in place.
    size_t out=0,j=0;
    for(auto selected:selection){
     while(j<hits.size()&&hits[j]<selected)++j;
     if(j<hits.size()&&hits[j]==selected)selection[out++]=selected;
    }
    selection.resize(out);
   }
   if(selection.empty())break;
  }
  if(columns.size()>1)result.matched+=selection.size();
 }
 return result;
}
} // namespace minicol
