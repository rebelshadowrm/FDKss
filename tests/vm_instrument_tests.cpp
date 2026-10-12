#include "vm_instrument.h"
#include "vm_pure.h"
#include "vm_snapshot.h"
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void check(bool value) { if (!value) throw std::runtime_error("VM instrumentation check failed"); }
void put(std::vector<unsigned char> &data, std::size_t at, std::uint32_t word) { std::memcpy(data.data() + at,&word,4); }
std::uint32_t word(const std::vector<unsigned char> &data, std::size_t at) { std::uint32_t word; std::memcpy(&word,data.data() + at,4); return word; }
struct Function { std::vector<unsigned char> code; unsigned char inputs = 0; };
std::vector<unsigned char> program(const std::vector<Function> &functions) {
    std::vector<unsigned char> data(1024);
    std::memcpy(data.data(),"#scp",4); put(data,4,24); put(data,8,static_cast<std::uint32_t>(functions.size())); put(data,12,900);
    for (std::size_t i = 0; i < functions.size(); ++i) {
        const auto at = 24 + i * 32;
        put(data,at,256 + static_cast<std::uint32_t>(i * 128)); data[at + 4] = functions[i].inputs;
        put(data,at + 12,800); put(data,at + 28,0xc0000280u + static_cast<std::uint32_t>(i * 16));
        data[640 + i * 16] = 'A' + static_cast<unsigned char>(i);
        std::memcpy(data.data() + 256 + i * 128,functions[i].code.data(),functions[i].code.size());
    }
    return data;
}
struct Interpreter {
    const std::vector<unsigned char> &data;
    std::uint32_t pc, result = 0, steps = 0;
    std::vector<std::uint32_t> stack{0x0fffffff};
    std::array<std::uint32_t,16> registers{};
    bool finished = false;
    bool slice(unsigned budget) {
        unsigned executed = 0;
        while (!finished) {
            check(pc + 4 <= data.size() && data[pc] == 36 && data[pc + 1] == 28 && !data[pc + 2] && !data[pc + 3]);
            if (executed == budget) return false;
            ++executed; ++steps; pc += 4;
            const auto op = data[pc++];
            if (op == 0) { check(data[pc++] == 4); stack.push_back(word(data,pc)); pc += 4; }
            else if (op == 9) stack.push_back(registers[data[pc++]]);
            else if (op == 10) { check(stack.size() > 1); registers[data[pc++]] = stack.back(); stack.pop_back(); }
            else if (op == 11 || op == 14 || op == 15) {
                const auto target = word(data,pc); pc += 4;
                bool take = true;
                if (op != 11) { check(stack.size() > 1); const auto value = pure_vm::integer(stack.back()); stack.pop_back(); take = op == 14 ? value != 0 : value == 0; }
                if (take) pc = target;
            } else if (op == 12) {
                std::uint16_t callee; std::memcpy(&callee,data.data() + pc,2);
                pc = word(data,word(data,4) + callee * 32);
            } else if (op == 13) {
                check(!stack.empty()); const auto returning = stack.back(); stack.pop_back();
                if (returning == 0x0fffffff) { check(stack.empty()); finished = true; result = registers[0]; }
                else { check(!stack.empty()); stack.pop_back(); pc = returning; }
            } else if (op >= 16 && op <= 33) {
                check(stack.size() >= 3); const auto right = stack.back(); stack.pop_back(); const auto left = stack.back(); stack.pop_back();
                std::uint32_t result; check(pure_vm::numeric_operation(op,left,right,result)); stack.push_back(result);
            } else throw std::runtime_error("test interpreter encountered unexpected opcode");
        }
        return true;
    }
};
}
int main() {
    try {
        const std::vector<unsigned char> loop{10,1,0,4,0,0,0,0x40,10,0,
            9,1,15,40,1,0,0,9,0,9,1,16,10,0,9,1,0,4,1,0,0,0x40,17,10,1,11,10,1,0,0,13};
        auto source = program({{loop,1}});
        budget_vm::Image image; std::string error;
        check(budget_vm::build(source,0,image,error));
        check(image.functions == 1 && image.addresses.size() == 15);
        // Preserve every original byte except the linked function's code field.
        auto restored = image.data; put(restored,24,256); restored.resize(source.size()); check(restored == source);
        for (unsigned quantum : {1u,7u,16u,128u}) {
            Interpreter vm{image.data,word(image.data,24)}; vm.stack.push_back(0x40000064);
            unsigned frames = 0;
            while (!vm.slice(quantum)) check(++frames < 2000);
            check(vm.result == 0x400013ba && vm.steps > 128 && frames > 0); // Sum 1..100 = 5050.
        }
        auto nested = program({{{0,4,0,0,0,0,0,4,15,1,0,0,12,1,0,9,0,0,4,2,0,0,0x40,16,10,0,13},0},
            {{0,4,5,0,0,0x40,10,0,13},0}});
        check(budget_vm::build(nested,0,image,error) && image.functions == 2);
        Interpreter vm{image.data,word(image.data,24)}; unsigned frames = 0;
        while (!vm.slice(1)) check(++frames < 32);
        check(vm.result == 0x40000007 && frames > 0);
        check(word(image.data,image.addresses.at(262) + 6) == image.addresses.at(271));
        auto bad = nested; put(bad,264,272); check(!budget_vm::build(bad,0,image,error)); // Forged packet.
        bad = nested; bad[269] = 255; check(!budget_vm::build(bad,0,image,error)); // Invalid callee.
        // Native commands and argument/reference instructions are decoded without executing them.
        auto native = program({{{4,0xfc,0xff,0xff,0xff,36,0,31,1,1,4,13},0}});
        check(budget_vm::build(native,0,image,error));
        native[262] = 28; check(!budget_vm::build(native,0,image,error)); // Reserved checkpoint group.
        auto global = program({{{7,1,0,0,0,10,0,13},0}});
        check(budget_vm::build(global,0,image,error));
        std::vector<std::uint32_t> values{0xc0000280,0x40000007,0x123456};
        check(budget_vm::pure_snapshot(global,image,values,error) && values[0]==0 && values[1]==0x40000007 && values[2]==0);
        values[1]=0xc0000280; check(!budget_vm::pure_snapshot(global,image,values,error));
        values[1]=0x9fe00000; check(!budget_vm::pure_snapshot(global,image,values,error)); // Tagged infinity.
        values.clear(); check(!budget_vm::pure_snapshot(global,image,values,error));
        auto skipped_native=program({{{0,4,0,0,0,0x40,14,16,1,0,0,13,0,0,0,0,36,0,31,0,13},0}});
        check(budget_vm::build(skipped_native,0,image,error));
        check(!budget_vm::pure_snapshot(skipped_native,image,values,error)); // Both outcomes checked, even a skipped native branch.
        bool has_timer=false;
        check(!budget_vm::pure_snapshot(skipped_native,image,values,error,true,&has_timer)); // get_timer cannot affect a prediction.
        bool has_read=false;
        check(budget_vm::pure_snapshot(skipped_native,image,values,error,false,&has_timer,true,&has_read) && has_read && !has_timer);
        auto valid_wait=program({{{36,0,28,0,0,4,1,0,0,0x40,36,0,30,1,1,4,36,0,29,0,13},0}});
        check(budget_vm::build(valid_wait,0,image,error));
        check(budget_vm::pure_snapshot(valid_wait,image,values,error,true,&has_timer) && has_timer);
        check(!budget_vm::pure_snapshot(valid_wait,image,values,error));
        auto mixed_timer=program({{{36,0,31,0,36,0,28,0,36,0,29,0,13},0}});
        budget_vm::Image mixed_image;
        check(budget_vm::build(mixed_timer,0,mixed_image,error));
        check(!budget_vm::pure_snapshot(mixed_timer,mixed_image,values,error,true,&has_timer,true,&has_read));
        for (auto [group,command,argc] : {std::array<unsigned,3>{1,30,1},{0,30,0},{0,29,1},{0,27,1},{0,31,0}}) {
            auto rejected=valid_wait;
            const auto address=image.addresses.begin()->first;
            rejected[address+1]=group; rejected[address+2]=command; rejected[address+3]=argc;
            check(!budget_vm::pure_snapshot(rejected,image,values,error,true,&has_timer));
        }
        for (auto op : {34,35,37,255}) {
            auto external = program({{{static_cast<unsigned char>(op),13},0}});
            check(!budget_vm::build(external,0,image,error));
        }
        auto branch = program({{{11,1,1,0,0,13},0}}); check(!budget_vm::build(branch,0,image,error)); // Operand overlap.
        branch = program({{{11,128,2,0,0,13},0}}); check(!budget_vm::build(branch,0,image,error)); // Metadata target.
        auto infinite = program({{{11,0,1,0,0},0}}); check(budget_vm::build(infinite,0,image,error));
        Interpreter hung{image.data,word(image.data,24)};
        for (int frame = 0; frame < 20; ++frame) check(!hung.slice(7));
        check(hung.steps == 140 && !hung.finished); // Suspension remains bounded even without EXIT.
        auto result_before = image.data;
        auto malformed = source; put(malformed,8,0xffffffff); check(!budget_vm::build(malformed,0,image,error) && image.data == result_before);
        std::cout << "Budgeted VM relocation, suspension and rejection checks passed\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
