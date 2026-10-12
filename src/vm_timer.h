#pragma once
#include "vm_pure.h"
namespace private_timer {
inline bool command(unsigned group, unsigned id, unsigned count) {
    return group==0 && ((id==28 || id==29) ? count==0 : id==30 && count==1);
}
inline bool read_command(unsigned group, unsigned id, unsigned count) {
    return group==0 && id==31 && count==0;
}
// These handlers do not change registers, arguments or shared engine state.
// Preflight skips elapsed waiting while validating the exact native ABI and
// start/stop sequence; instruction budgeting still bounds the selected path.
struct Preview {
    bool active=false, used=false, read=false;
    bool read_result(unsigned group,unsigned id,const std::uint32_t *,unsigned count,std::uint32_t &result) {
        if (used || !read_command(group,id,count)) return false;
        read=true;result=0x80000000u;return true; // New inactive private Thread: elapsed float zero.
    }
    bool invoke(unsigned group,unsigned id,const std::uint32_t *args,unsigned count) {
        if (read || !command(group,id,count)) return false;
        if (id==28) { active=used=true; return true; }
        if (id==29) { active=false;used=true; return true; }
        if (!active || !args || !pure_vm::scalar(args[0])) return false;
        const auto kind=args[0]>>30;
        const double duration=kind==1 ? static_cast<double>(pure_vm::integer(args[0])) :
            kind==2 ? static_cast<double>(std::bit_cast<float>(args[0]<<2)) : -1;
        const bool valid=duration>=0 && duration<=30;
        if (valid) used=true;
        return valid;
    }
};
}
