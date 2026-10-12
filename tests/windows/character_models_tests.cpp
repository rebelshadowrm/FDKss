#include "character_models.h"
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

int main() {
    const auto check=[](bool good,std::source_location at=std::source_location::current()) {
        if(!good)throw std::runtime_error("character model check at "+std::to_string(at.line()));
    };
    try {
        for(const auto &p:{character_models::first,character_models::second,character_models::object_first,character_models::object_second}) {
            const bool object=p.link==0x190;
            const auto &primary=object ? (p.maximum_version==4 ? character_models::first : character_models::second) : p;
            constexpr std::uintptr_t image=0x100000000ull;
            alignas(16) std::array<unsigned char,0x400> actor_bytes{},model_bytes{};
            const auto actor=reinterpret_cast<std::uintptr_t>(actor_bytes.data());
            const auto model=reinterpret_cast<std::uintptr_t>(model_bytes.data());
            const auto put=[](std::uintptr_t at,const auto &value){std::memcpy(reinterpret_cast<void *>(at),&value,sizeof(value));};
            put(actor,image+p.actor_table);put(actor+p.link,model);put(model,image+p.model_table);
            put(model+0x10,std::uint32_t{0x204c444d});put(model+0x14,std::uint32_t{4});
            std::memcpy(reinterpret_cast<void *>(model+0x1c),"hero",5);
            put(model+0x258,std::uint64_t{3});put(model+0x288,std::uint64_t{247});
            bool absent=false,expired=false,wrong_class=false,changed_class=false,changed_name=false;
            unsigned resolutions=0,name_reads=0,class_reads=0;
            unsigned replace_at=0;
            const auto resolve=[&](std::string_view) {
                ++resolutions;
                return absent || expired || (replace_at && resolutions>=replace_at) ? std::uintptr_t{0} : actor;
            };
            const auto read=[&](std::uintptr_t at,void *out,std::size_t size) {
                if(!((at>=actor && size<=actor_bytes.size() && at-actor<=actor_bytes.size()-size) ||
                     (at>=model && size<=model_bytes.size() && at-model<=model_bytes.size()-size)))return false;
                if(at==model+0x1c && ++name_reads==2 && changed_name)model_bytes[0x1c]='x';
                std::memcpy(out,reinterpret_cast<void *>(at),size);return true;
            };
            const auto model_class=[&](std::uintptr_t value){return value==model && !wrong_class && !(changed_class && ++class_reads==2);};
            const auto string=[](const char *s){fdkss_value v{};v.type=FDKSS_STRING;v.string=s;v.string_size=std::strlen(s);return v;};
            fdkss_value input=string("player");char error[256]{};
            struct Result {std::string name;std::vector<fdkss_value> values;};Result result;
            const auto capture=+[](void *opaque,const fdkss_value *value)->int {
                auto &out=*static_cast<Result *>(opaque);out.values.push_back(*value);
                if(value->type==FDKSS_STRING)out.name.assign(value->string,value->string_size);
                return 0;
            };
            const auto invoke=[&](std::size_t count=1) {
                resolutions=name_reads=class_reads=0;result={};
                return character_models::snapshot(image,primary,resolve,read,model_class,&input,count,capture,&result,error,sizeof(error),object ? &p : nullptr);
            };
            check(!invoke() && result.name=="hero" && result.values.size()==4 && result.values[1].integer==4 &&
                result.values[2].integer==3 && result.values[3].integer==247);
            if(object)check(character_models::snapshot(image,primary,resolve,read,model_class,&input,1,capture,&result,error,sizeof(error))!=0);
            const auto copied=result.name;model_bytes[0x1c]='H';check(copied=="hero");model_bytes[0x1c]='h';
            input=string("@fixture");check(!invoke() && result.name=="hero");
            expired=true;check(invoke()!=0 && result.values.empty());expired=false;
            input=string("player");absent=true;check(!invoke() && result.values.size()==1 && result.values[0].type==FDKSS_NIL);absent=false;
            put(actor+p.link,std::uintptr_t{0});check(!invoke() && result.values[0].type==FDKSS_NIL);put(actor+p.link,model);
            for(const auto reference:{"framework","player_model","0x1234",""}){input=string(reference);check(invoke()!=0 && result.values.empty());}
            input=string("player");check(invoke(0)!=0 && invoke(2)!=0);
            for(const auto tag:{FDKSS_NIL,FDKSS_INTEGER,FDKSS_BOOLEAN}){input.type=tag;check(invoke()!=0);}input=string("player");
            for(const auto &fault:std::array<std::pair<std::uintptr_t,std::uint64_t>,6>{{
                {actor,image+p.actor_table+8},{actor+p.link,1},{model,image+p.model_table+8},
                {model+0x258,10001},{model+0x288,0x100000001ull},{model+0x10,0}}}) {
                std::uint64_t previous=0;std::memcpy(&previous,reinterpret_cast<void *>(fault.first),8);
                put(fault.first,fault.second);check(invoke()!=0 && result.values.empty());put(fault.first,previous);
            }
            put(model+0x14,std::uint32_t{5});check(p.maximum_version==5 ? !invoke() && result.values[1].integer==5 : invoke()!=0);
            put(model+0x14,std::uint32_t{6});check(invoke()!=0);put(model+0x14,std::uint32_t{4});
            wrong_class=true;check(invoke()!=0);wrong_class=false;
            changed_class=true;check(invoke()!=0);changed_class=false;
            changed_name=true;check(invoke()!=0 && result.values.empty());changed_name=false;model_bytes[0x1c]='h';
            for(const auto at:{2u,3u}){replace_at=at;check(invoke()!=0 && result.values.empty());}replace_at=0;
            std::memset(reinterpret_cast<void *>(model+0x1c),'a',64);check(invoke()!=0);
            model_bytes[0x5b]=0;check(!invoke() && result.name.size()==63);
            put(model+0x258,std::uint64_t{0});put(model+0x288,std::uint64_t{10000});check(!invoke() && result.values[2].integer==0 && result.values[3].integer==10000);
        }
        std::cout<<"Both character Model profiles, copied fields, expiry/chain/type/layout guards passed\n";
    }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
