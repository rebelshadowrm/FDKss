#pragma once
#include "vm_instrument.h"
#include "vm_pure.h"
#include "vm_timer.h"
namespace budget_vm {
// A copied pure job keeps only numeric/nil globals used anywhere in its linked
// graph. Unrelated engine strings/handles are discarded before creating a thread.
inline bool pure_snapshot(std::span<const unsigned char> source, const Image &image,
    std::vector<std::uint32_t> &globals, std::string &error, bool timers=false, bool *has_timer=nullptr,
    bool timer_reads=false, bool *has_timer_read=nullptr) {
    std::vector<bool> used(globals.size());
    bool native_timer=false, native_timer_read=false;
    for (const auto &[address,relocated] : image.addresses) {
        (void)relocated;
        if (address>=source.size()) { error="async pure instruction is outside source"; return false; }
        const auto op=source[address];
        if (op==36 && timer_reads && source.size()-address>=4 &&
            private_timer::read_command(source[address+1],source[address+2],source[address+3])) {
            native_timer_read=true; continue;
        }
        if (op==36 && timers) {
            if (source.size()-address<4 || !private_timer::command(source[address+1],source[address+2],source[address+3])) {
                error="async graph contains an unsupported native timer command"; return false;
            }
            native_timer=true; continue;
        }
        if (!(op<=2 || op==7 || (op>=9 && op<=33) || op==38)) {
            error="async pure graph contains an unsupported opcode"; return false;
        }
        if (op==7) {
            std::uint32_t index=0;
            if (source.size()-address<5) { error="async pure global instruction is truncated"; return false; }
            std::memcpy(&index,source.data()+address+1,4);
            if (index>=globals.size() || !pure_vm::scalar(globals[index]) || (globals[index]>>30)==3) {
                error="async pure global reads require a valid nil or numeric snapshot"; return false;
            }
            used[index]=true;
        }
    }
    if (native_timer && native_timer_read) {error="private timer reads cannot be combined with timer waits";return false;}
    for (std::size_t i=0;i<globals.size();++i) if (!used[i]) globals[i]=0;
    if (has_timer) *has_timer=native_timer;
    if (has_timer_read) *has_timer_read=native_timer_read;
    error.clear(); return true;
}
}
