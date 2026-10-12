#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "character_snapshots.h"
#include <memory>
#include <stdexcept>
#include <vector>
#include <string>
#include <source_location>
namespace {
void check(bool value,std::source_location at=std::source_location::current()){
    if(!value)throw std::runtime_error("condition snapshot guard at line "+std::to_string(at.line()));
}
template<class T>void store(std::uintptr_t address,const T &value){std::memcpy(reinterpret_cast<void *>(address),&value,sizeof(value));}
struct Result {std::vector<fdkss_value_type> types;std::int64_t count=0;std::string bytes;std::vector<std::int64_t> integers;};
int FDKSS_CALL capture(void *sink,const fdkss_value *value){
    auto &r=*static_cast<Result *>(sink);r.types.push_back(value->type);
    if(value->type==FDKSS_INTEGER){r.count=value->integer;r.integers.push_back(value->integer);}
    if(value->type==FDKSS_STRING)r.bytes.assign(value->string,value->string_size);
    return 0;
}
}
int main(){try{
    auto image_storage=std::make_unique<unsigned char[]>(0xb00000);
    const auto image=reinterpret_cast<std::uintptr_t>(image_storage.get());
    for(const bool second:{false,true}){
        const auto &p=second ? player_conditions::sky2 : player_conditions::sky1;
        const auto &a=second ? player_resources::sky2 : player_resources::sky1;
        const auto nibble=[](char c){return c<='9' ? c-'0' : c-'a'+10;};
        for(std::size_t i=0;i<p.code.size()/2;++i)image_storage[p.reader+i]=static_cast<unsigned char>(nibble(p.code[2*i])*16+nibble(p.code[2*i+1]));
        std::array<unsigned char,0x2000> actor{};std::array<unsigned char,0x600> link{};
        std::array<unsigned char,0x940> manager{};std::array<unsigned char,128*0x48> records{};
        const auto object=reinterpret_cast<std::uintptr_t>(actor.data()),l=reinterpret_cast<std::uintptr_t>(link.data()),m=reinterpret_cast<std::uintptr_t>(manager.data()),data=reinterpret_cast<std::uintptr_t>(records.data());
        store(object,image+a.table);store(object+a.link,l);store(l+p.manager,m);store(m,object);store(m+0x908,data);store(m+0x910,std::uint32_t{2});
        // +914 is not independently proven capacity by these reader fragments.
        store(m+0x914,std::uint32_t{0xdeadbeef});
        for(std::size_t i=0;i<records.size();++i)records[i]=static_cast<unsigned char>(i);
        store(data,std::uint32_t{27});store(data+0x48,std::uint32_t{22});
        Result result;char error[256]{};const auto current=[&]{return object;};
        auto run=[&](auto resolve,std::size_t count=0){result={};return player_conditions::snapshot(image,a,p,resolve,count,capture,&result,error,sizeof(error));};
        check(!run(current) && result.count==2 && result.bytes==std::string(reinterpret_cast<char *>(records.data()),2*0x48));
        // Reuse these verified chains through reference-based read-only APIs.
        for(std::size_t i=0;i<a.code.size()/2;++i)image_storage[a.getter+i]=static_cast<unsigned char>(nibble(a.code[2*i])*16+nibble(a.code[2*i+1]));
        std::array<unsigned char,0x40> status{};const auto status_address=reinterpret_cast<std::uintptr_t>(status.data());
        store(l,status_address);const std::array<std::uint32_t,6> stats{7,10,4,20,99,200};
        std::memcpy(status.data()+0xc,stats.data(),sizeof(stats));
        fdkss_value reference{};reference.type=FDKSS_STRING;reference.string="@fixture";reference.string_size=8;
        const auto resolve=[&](std::string_view name){return name=="@fixture" || name=="player" ? object : std::uintptr_t{0};};
        auto referenced=[&](auto resolver,bool conditions,std::size_t count=1){result={};return character_snapshots::snapshot(image,a,p,conditions,resolver,&reference,count,capture,&result,error,sizeof(error));};
        check(!referenced(resolve,true) && result.count==2 && result.bytes==std::string(reinterpret_cast<char *>(records.data()),144));
        check(!referenced(resolve,false) && result.integers==std::vector<std::int64_t>{7,10,4,20,99,200});
        check(referenced(resolve,true,0) && result.types.empty());
        check(referenced(resolve,false,2) && result.types.empty());
        reference.string="framework";reference.string_size=9;check(referenced(resolve,true) && result.types.empty());
        reference.string="@stale";reference.string_size=6;check(referenced(resolve,true) && result.types.empty());
        reference.string="@fixture";reference.string_size=8;
        reference.type=FDKSS_INTEGER;check(referenced(resolve,false) && result.types.empty());reference.type=FDKSS_STRING;
        store(object,image+a.table+8);check(referenced(resolve,true) && result.types.empty());store(object,image+a.table);
        unsigned reference_reads=0;check(referenced([&](std::string_view){return ++reference_reads==1 ? object : std::uintptr_t{0};},true) && result.types.empty());
        reference_reads=0;check(referenced([&](std::string_view){return ++reference_reads==1 ? object : std::uintptr_t{0};},false) && result.types.empty());
        store(object+a.link,std::uintptr_t{0});check(!referenced(resolve,true) && result.types==std::vector<fdkss_value_type>{FDKSS_NIL});store(object+a.link,l);
        check(!run(current));
        auto copied=result.bytes;records[10]^=1;check(copied[10]!=static_cast<char>(records[10]));
        check(run(current,1) && result.types.empty());
        check(!run([]{return std::uintptr_t{0};}) && result.types==std::vector<fdkss_value_type>{FDKSS_NIL});
        store(object,image+a.table+8);check(run(current) && result.types.empty());store(object,image+a.table);
        store(l+p.manager,std::uintptr_t{0});check(!run(current) && result.types[0]==FDKSS_NIL);store(l+p.manager,m);
        store(m,object+8);check(run(current) && result.types.empty());store(m,object);
        image_storage[p.reader+7]^=1;check(run(current) && result.types.empty());image_storage[p.reader+7]^=1;
        unsigned calls=0;check(run([&]{return ++calls==1 ? object : std::uintptr_t{0};}) && result.types.empty());
        calls=0;check(run([&]{if(++calls==2)records[0]^=1;return object;}) && result.types.empty());
        calls=0;check(run([&]{if(++calls==2)store(m+0x910,std::uint32_t{1});return object;}) && result.types.empty());
        store(m+0x910,std::uint32_t{129});check(run(current) && result.types.empty());
        store(m+0x910,std::uint32_t{0xffffffffu});check(run(current) && result.types.empty());
        store(m+0x910,std::uint32_t{128});check(!run(current) && result.bytes.size()==128*0x48);
        store(m+0x910,std::uint32_t{0});store(m+0x908,std::uintptr_t{0});check(!run(current) && result.count==0 && result.bytes.empty());
        store(m+0x910,std::uint32_t{1});check(run(current) && result.types.empty());
        auto inaccessible=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS);check(inaccessible!=nullptr);
        store(m+0x908,reinterpret_cast<std::uintptr_t>(inaccessible));check(run(current) && result.types.empty());
        store(l+p.manager,reinterpret_cast<std::uintptr_t>(inaccessible));check(run(current) && result.types.empty());
        check(VirtualFree(inaccessible,0,MEM_RELEASE)!=0);
    }
    return 0;
}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
