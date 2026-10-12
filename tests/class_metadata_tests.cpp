#include "../src/class_metadata.h"
#include <vector>
#include <stdexcept>
#include <iostream>
#include <source_location>
int main() {
    try {
        std::vector<unsigned char> data(4096);
        constexpr std::uintptr_t image=0x10000,table=image+0x100;
        auto put=[&](unsigned offset,const auto &value){std::memcpy(data.data()+offset,&value,sizeof(value));};
        auto text=[&](unsigned offset,const char *s){std::strcpy(reinterpret_cast<char *>(data.data()+offset+16),s);};
        const std::uintptr_t locator=image+0x200;put(0xf8,locator);
        std::array<std::uint32_t,6> col{1,0,0,0x800,0x400,0x200};put(0x200,col);
        std::array<std::uint32_t,4> hierarchy{0,3,3,0x500};put(0x400,hierarchy);
        std::array<std::uint32_t,3> bases{0x600,0x620,0x640};put(0x500,bases);
        std::array<std::uint32_t,6> derived{0x800,2,0,0xffffffff,0,0};put(0x600,derived);
        std::array<std::uint32_t,6> first{0x900,0,24,0xffffffff,0,4};put(0x620,first);
        std::array<std::uint32_t,6> virt{0xa00,0,0,8,16,16};put(0x640,virt);
        text(0x800,".?AVDerived@test@@");text(0x900,".?AVBase@test@@");text(0xa00,".?AVVirtual@test@@");
        bool unreadable=false;
        auto read=[&](std::uintptr_t at,void *out,std::size_t n){if(unreadable || at<image || at-image>data.size() || n>data.size()-(at-image))return false;std::memcpy(out,data.data()+at-image,n);return true;};
        auto check=[](bool b,std::source_location where=std::source_location::current()){if(!b)throw std::runtime_error("class metadata check failed at line "+std::to_string(where.line()));};
        class_metadata::Row row;
        auto inspect=[&](unsigned i){return class_metadata::inspect(image,data.size(),table,i,read,row);};
        check(inspect(0) && row.name==".?AVDerived@test@@" && row.base_name==row.name && row.count==3 && row.contained==2);
        check(inspect(1) && row.member_offset==24 && row.vbtable_offset==-1 && row.attributes==4);
        check(inspect(2) && row.vbtable_offset==8 && row.virtual_displacement==16 && row.flags==3);
        check(!inspect(3));unreadable=true;check(!inspect(0));unreadable=false;
        col[5]=0x201;put(0x200,col);check(!inspect(0));col[5]=0x200;put(0x200,col);
        hierarchy[2]=1025;put(0x400,hierarchy);check(!inspect(0));hierarchy[2]=3;
        hierarchy[3]=4092;put(0x400,hierarchy);check(!inspect(0));hierarchy[3]=0x500;put(0x400,hierarchy);
        first[1]=2;put(0x620,first);check(!inspect(1));first[1]=0;put(0x620,first);
        derived[1]=0;put(0x600,derived);check(!inspect(0));derived[1]=2;put(0x600,derived);
        bases[1]=4090;put(0x500,bases);check(!inspect(1));bases[1]=0x620;put(0x500,bases);
        const auto saved=data;
        std::memset(data.data()+0x900+16,'x',256);check(!inspect(1));data=saved;
        check(inspect(1));const auto copied=row.base_name;text(0x900,".?AVChanged@test@@");check(row.base_name==copied);
        check(!class_metadata::inspect(image,data.size(),image+4,0,read,row));
        std::cout<<"Class metadata checks passed\n";
    } catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
