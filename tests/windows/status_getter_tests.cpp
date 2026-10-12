#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "status_getter.h"
#include <vector>
#include <stdexcept>
#include <source_location>
#include <string>
namespace {
void check(bool value,std::source_location at=std::source_location::current()) {
    if(!value)throw std::runtime_error("status getter check at line "+std::to_string(at.line()));
}
template<class T>void store(std::uintptr_t address,const T &value){std::memcpy(reinterpret_cast<void *>(address),&value,sizeof(value));}
int FDKSS_CALL capture(void *sink,const fdkss_value *value){static_cast<std::vector<fdkss_value> *>(sink)->push_back(*value);return 0;}
}
int main(){try{
    auto allocation=VirtualAlloc(nullptr,0xb00000,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);check(allocation!=nullptr);
    struct Cleanup{void *p;~Cleanup(){VirtualFree(p,0,MEM_RELEASE);}} cleanup{allocation};
    const auto image=reinterpret_cast<std::uintptr_t>(allocation);
    auto code=static_cast<unsigned char *>(allocation);
    unsigned native_cases=0;
    for(const auto &p:{player_resources::sky1,player_resources::sky2}) {
        const auto nibble=[](char c){return c<='9' ? c-'0' : c-'a'+10;};
        for(std::size_t i=0;i<p.code.size()/2;++i)code[p.getter+i]=static_cast<unsigned char>(nibble(p.code[2*i])*16+nibble(p.code[2*i+1]));
        check(FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void *>(image+p.getter),p.code.size()/2)!=0);
        alignas(16) std::array<unsigned char,0x2000> actor{};
        std::array<std::uint32_t,64> status{};
        const auto object=reinterpret_cast<std::uintptr_t>(actor.data());
        std::uintptr_t status_address=reinterpret_cast<std::uintptr_t>(status.data());
        const auto link=reinterpret_cast<std::uintptr_t>(&status_address);
        store(object,image+p.table);store(object+p.link,link);
        std::vector<fdkss_value> values;char error[256]{};
        fdkss_value args[2]{};args[0].type=FDKSS_STRING;args[0].string="@fixture";args[0].string_size=8;args[1].type=FDKSS_INTEGER;
        const auto current=[&](std::string_view reference){return reference=="@fixture" ? object : std::uintptr_t{0};};
        auto run=[&](auto resolve,std::size_t count=2){values.clear();return status_getter::invoke(image,p,resolve,args,count,capture,&values,error,sizeof(error));};
        std::uint32_t random=106080;
        for(const auto &field:status_getter::fields) {
            args[1].integer=field.selector;
            for(unsigned sample=0;sample<133;++sample) {
                for(auto &word:status){random^=random<<13;random^=random>>17;random^=random<<5;word=random;}
                constexpr std::array<std::uint32_t,5> edges{0,1,0x7fffffffu,0x80000000u,0xffffffffu};
                if(sample<edges.size())status[field.offset/4]=edges[sample];
                const auto before=status;
                check(!run(current) && values.size()==1 && values[0].type==FDKSS_INTEGER &&
                    values[0].integer==status[field.offset/4] && status==before);++native_cases;
            }
        }
        args[1].integer=7;
        check(run(current,1) && values.empty());check(run(current,3) && values.empty());
        for(const std::int64_t selector:{-1ll,1ll,8ll,14ll,79ll,85ll,4294967296ll}){args[1].integer=selector;check(run(current) && values.empty());}
        args[1].integer=7;args[1].type=FDKSS_NUMBER;args[1].number=7;check(run(current) && values.empty());args[1].type=FDKSS_INTEGER;
        args[0].string="framework";args[0].string_size=9;check(run(current) && values.empty());
        args[0].string="@stale";args[0].string_size=6;check(run(current) && values.empty());args[0].string="@fixture";args[0].string_size=8;
        store(object,image+p.table+8);check(run(current) && values.empty());store(object,image+p.table);
        unsigned calls=0;check(run([&](std::string_view){return ++calls==1 ? object : std::uintptr_t{0};}) && values.empty());
        calls=0;check(run([&](std::string_view){return ++calls<3 ? object : std::uintptr_t{0};}) && values.empty());
        calls=0;check(run([&](std::string_view){if(++calls==2)status[3]^=1;return object;}) && values.empty());
        calls=0;check(run([&](std::string_view){if(++calls==2)code[p.getter+p.code.size()/2-1]^=1;return object;}) && values.empty());
        code[p.getter+p.code.size()/2-1]^=1;
        code[p.getter+7]^=1;check(run(current) && values.empty());code[p.getter+7]^=1;
        const auto saved=status_address;status_address=0;check(!run(current) && values.size()==1 && values[0].type==FDKSS_NIL);status_address=saved;
        store(object+p.link,std::uintptr_t{0});check(!run(current) && values.size()==1 && values[0].type==FDKSS_NIL);store(object+p.link,link);
        status_address=saved+1;check(run(current) && values.empty());status_address=saved;
        auto page=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);check(page!=nullptr);
        std::memcpy(page,status.data(),sizeof(status));status_address=reinterpret_cast<std::uintptr_t>(page);
        DWORD previous=0;check(VirtualProtect(page,4096,PAGE_READONLY,&previous)!=0);
        check(!run(current) && !std::memcmp(page,status.data(),sizeof(status)));
        check(VirtualProtect(page,4096,PAGE_NOACCESS,&previous)!=0);check(run(current) && values.empty());
        status_address=saved;check(VirtualFree(page,0,MEM_RELEASE)!=0);
        check(VirtualProtect(reinterpret_cast<void *>(image+p.getter),1,PAGE_READWRITE,&previous)!=0);
        check(run(current) && values.empty());check(VirtualProtect(reinterpret_cast<void *>(image+p.getter),1,previous,&previous)!=0);
    }
    std::printf("%u original native status getter cases passed\n",native_cases);return 0;
}catch(const std::exception &error){std::fprintf(stderr,"%s\n",error.what());return 1;}}
