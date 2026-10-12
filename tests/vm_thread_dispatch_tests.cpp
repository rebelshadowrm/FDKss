#include "vm_thread_dispatch.h"
#include <iostream>
#include <stdexcept>
struct Thread { const std::uintptr_t *table; unsigned commands=0, callbacks=0, completions=0, errors=0; };
void command(Thread *t) { ++t->commands; }
void observer(Thread *t) { ++t->callbacks; command(t); }
void complete(Thread *t) { ++t->completions; }
void report(Thread *t) { ++t->errors; }
void unused(Thread *) {}
void require(bool condition) { if (!condition) throw std::runtime_error("private Thread dispatch fixture failed"); }
void invoke(Thread &t, unsigned slot) { reinterpret_cast<void (*)(Thread *)>(t.table[slot])(&t); }
int main() {
    try {
        budget_vm::ThreadMethods shared{}, isolated{};
        shared.fill(reinterpret_cast<std::uintptr_t>(&unused));
        shared[4]=reinterpret_cast<std::uintptr_t>(&observer);
        shared[6]=reinterpret_cast<std::uintptr_t>(&complete); shared[7]=reinterpret_cast<std::uintptr_t>(&report);
        require(budget_vm::private_dispatch(shared,reinterpret_cast<std::uintptr_t>(&command),isolated));
        Thread private_thread{isolated.data()}, game_thread{shared.data()};
        // Match opcode36 and outer EXIT's independently traced virtual slots.
        for (unsigned slice=0;slice<32;++slice) invoke(private_thread,4);
        invoke(private_thread,6); invoke(private_thread,7);
        require(private_thread.commands==32 && private_thread.callbacks==0 && private_thread.completions==1 && private_thread.errors==1);
        invoke(game_thread,4); require(game_thread.commands==1 && game_thread.callbacks==1);
        for (unsigned i=0;i<8;++i) if (i!=4) require(isolated[i]==shared[i]);
        require(!budget_vm::private_dispatch(std::span(shared).first(5),reinterpret_cast<std::uintptr_t>(&command),isolated));
        shared[6]=0; require(!budget_vm::private_dispatch(shared,reinterpret_cast<std::uintptr_t>(&command),isolated));
        require(budget_vm::private_suspend(5,28,0,0,7,7,false));
        require(!budget_vm::private_suspend(5,28,0,1,6,7,true));
        require(budget_vm::private_suspend(5,0,30,6,1,7,true));
        require(budget_vm::private_suspend(5,0,30,7,0,7,true)); // Redispatch after a native wait.
        require(!budget_vm::private_suspend(5,0,30,7,0,7,false)); // Pure jobs cannot wait.
        require(!budget_vm::private_suspend(5,0,27,7,0,7,true)); // Other waits need their own ABI.
        require(!budget_vm::private_suspend(1,0,30,7,0,7,true));
        require(!budget_vm::private_suspend(4,0,30,7,0,7,true));
        require(!budget_vm::private_suspend(5,0,30,7,8,7,true));
        std::cout << "Private dispatch isolation, completion and diagnostic virtual ABI passed\n"; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
