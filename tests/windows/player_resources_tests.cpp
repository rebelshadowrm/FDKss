#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "resource_properties.h"
#include <memory>
#include <stdexcept>
#include <vector>
namespace {
void check(bool value){if(!value)throw std::runtime_error("player resource guard failed");}
template<class T> void store(std::uintptr_t at,const T &value){std::memcpy(reinterpret_cast<void *>(at),&value,sizeof(value));}
int FDKSS_CALL capture(void *sink,const fdkss_value *value){
    static_cast<std::vector<fdkss_value> *>(sink)->push_back(*value);return 0;
}
}
int main(){
    try {
        struct Image {
            unsigned char *data=static_cast<unsigned char *>(VirtualAlloc(nullptr,0xb00000,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));
            ~Image(){if(data)VirtualFree(data,0,MEM_RELEASE);}
        } allocation;
        auto *storage=allocation.data;check(storage!=nullptr);
        const auto image=reinterpret_cast<std::uintptr_t>(storage);
        for(const auto &p:{player_resources::sky1,player_resources::sky2}) {
            const auto nibble=[](char c){return c<='9' ? c-'0' : c-'a'+10;};
            for(std::size_t i=0;i<p.code.size()/2;++i)storage[p.getter+i]=static_cast<unsigned char>(nibble(p.code[2*i])*16+nibble(p.code[2*i+1]));
            std::array<unsigned char,0x2000> actor{};
            const auto address=reinterpret_cast<std::uintptr_t>(actor.data());
            std::uintptr_t link=0,status=0;
            std::array<std::uint32_t,9> fields{0,108,1216978,25997,25997,0,2375,199,200};
            link=reinterpret_cast<std::uintptr_t>(&status);status=reinterpret_cast<std::uintptr_t>(fields.data());
            store(address,image+p.table);store(address+p.link,link);
            std::vector<fdkss_value> values;char error[256]{};
            auto run=[&](auto resolver,std::size_t count=0){values.clear();return player_resources::snapshot(image,p,resolver,count,capture,&values,error,sizeof(error));};
            const auto current=[&]{return address;};
            check(!run(current) && values.size()==6 && values[0].integer==25997 && values[2].integer==0 && values[4].integer==199);
            const auto copied=values;
            fields[3]=12;check(copied[0].integer==25997);check(!run(current) && values[0].integer==12);
            check(run(current,1) && values.empty());
            check(!run([]{return std::uintptr_t{0};}) && values.size()==1 && values[0].type==FDKSS_NIL);
            store(address,image+p.table+8);check(run(current) && values.empty());store(address,image+p.table);
            fields[3]=fields[4]+1;check(run(current) && values.empty());fields[3]=0;
            fields[4]=0x80000000u;check(run(current) && values.empty());fields[4]=25997;
            const auto saved=storage[p.getter+7];storage[p.getter+7]^=1;
            check(run(current) && values.empty());storage[p.getter+7]=saved;
            unsigned calls=0;
            check(run([&]{return ++calls==1 ? address : std::uintptr_t{0};}) && values.empty());
            calls=0;
            check(run([&]{if(++calls==2)fields[7]=198;return address;}) && values.empty());
            calls=0;const auto original_status=status;
            check(run([&]{if(++calls==2)status=0;return address;}) && values.empty());status=original_status;
            status=0;check(!run(current) && values.size()==1 && values[0].type==FDKSS_NIL);status=original_status;
            store(address+p.link,std::uintptr_t{0});check(!run(current) && values.size()==1 && values[0].type==FDKSS_NIL);store(address+p.link,link);
            auto inaccessible=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS);check(inaccessible!=nullptr);
            status=reinterpret_cast<std::uintptr_t>(inaccessible);check(run(current) && values.empty());status=original_status;
            store(address+p.link,reinterpret_cast<std::uintptr_t>(inaccessible));check(run(current) && values.empty());store(address+p.link,link);
            check(VirtualFree(inaccessible,0,MEM_RELEASE)!=0);
            const auto &setter=p.link==player_resources::sky1.link ? player_resources::sky1_setter : player_resources::sky2_setter;
            for(std::size_t i=0;i<setter.code.size()/2;++i)storage[setter.entry+i]=static_cast<unsigned char>(nibble(setter.code[2*i])*16+nibble(setter.code[2*i+1]));
            check(FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void *>(image+setter.entry),setter.code.size()/2)!=0);
            fdkss_value args[2]{};args[0].type=FDKSS_STRING;args[1].type=FDKSS_INTEGER;
            auto edit=[&](auto resolver,std::size_t count=2){values.clear();return player_resources::edit(image,p,setter,resolver,args,count,capture,&values,error,sizeof(error));};
            for(const auto name: {"hp","ep","cp"}) {
                args[0].string=name;args[0].string_size=2;
                const std::size_t index=name[0]=='h' ? 3 : name[0]=='e' ? 5 : 7;
                for(const std::uint32_t maximum:{0u,1u,200u,25997u,0x7fffffffu}) {
                    fields={0,108,1216978,0,maximum,0,maximum,0,maximum};
                    for(const std::int64_t request:{std::int64_t(-2147483648),std::int64_t(-1),std::int64_t(0),std::int64_t(1),std::int64_t(100),std::int64_t(2147483647)}) {
                        auto expected=fields;const auto result=request<0 ? 0 : request>maximum ? maximum : static_cast<std::uint32_t>(request);
                        expected[index]=static_cast<std::uint32_t>(result);args[1].integer=request;
                        check(!edit(current) && values.size()==1 && values[0].integer==result && fields==expected);
                    }
                }
            }
            fields={0,108,1216978,12,25997,0,2375,198,200};
            // Property aliases retain reference validation and the same native clamp semantics.
            const auto resolve_reference=[&](std::string_view reference){return reference=="@fixture" ? address : std::uintptr_t{0};};
            fdkss_value property_args[3]{};property_args[0].type=property_args[1].type=FDKSS_STRING;
            property_args[0].string="@fixture";property_args[0].string_size=8;property_args[1].string="hp";property_args[1].string_size=2;
            property_args[2].type=FDKSS_INTEGER;property_args[2].integer=11;values.clear();
            check(!resource_properties::set(image,p,setter,resolve_reference,property_args,capture,&values,error,sizeof(error)) && fields[3]==11);
            property_args[2].integer=12;values.clear();check(!resource_properties::set(image,p,setter,resolve_reference,property_args,capture,&values,error,sizeof(error)) && fields[3]==12);
            property_args[1].string="max_hp";property_args[1].string_size=6;values.clear();
            check(resource_properties::set(image,p,setter,resolve_reference,property_args,capture,&values,error,sizeof(error)) && values.empty() && fields[3]==12);
            property_args[1].string="hp";property_args[1].string_size=2;property_args[0].string="@stale";property_args[0].string_size=6;
            check(resource_properties::set(image,p,setter,resolve_reference,property_args,capture,&values,error,sizeof(error)) && fields[3]==12);
            const auto untouched=fields;args[0].string="hp";args[0].string_size=2;args[1].integer=10;
            check(edit(current,1) && fields==untouched && values.empty());
            args[0].string="HP";check(edit(current) && fields==untouched);args[0].string="hp";
            args[1].type=FDKSS_NUMBER;args[1].number=10;check(edit(current) && fields==untouched);args[1].type=FDKSS_INTEGER;
            args[1].integer=2147483648ll;check(edit(current) && fields==untouched);args[1].integer=-2147483649ll;check(edit(current) && fields==untouched);args[1].integer=10;
            check(edit([]{return std::uintptr_t{0};}) && fields==untouched);
            calls=0;check(edit([&]{return ++calls==1 ? address : std::uintptr_t{0};}) && fields==untouched);
            const auto pin_last=setter.entry+setter.code.size()/2-1;storage[pin_last]^=1;
            check(edit(current) && fields==untouched);storage[pin_last]^=1;
            DWORD previous=0;check(VirtualProtect(reinterpret_cast<void *>(image+setter.entry),1,PAGE_READWRITE,&previous)!=0);
            check(edit(current) && fields==untouched);check(VirtualProtect(reinterpret_cast<void *>(image+setter.entry),1,previous,&previous)!=0);
            auto readonly=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);check(readonly!=nullptr);
            std::memcpy(readonly,fields.data(),sizeof(fields));status=reinterpret_cast<std::uintptr_t>(readonly);
            check(VirtualProtect(readonly,4096,PAGE_READONLY,&previous)!=0);
            check(edit(current) && !std::memcmp(readonly,fields.data(),sizeof(fields)));
            status=original_status;check(VirtualFree(readonly,0,MEM_RELEASE)!=0);
        }
        return 0;
    }catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
