#include "vm_pure.h"
#include "vm_timer.h"
#include <iostream>
#include <stdexcept>
#include <vector>

void check(bool value) { if (!value) throw std::runtime_error("pure VM preflight failed"); }
namespace {
struct TestFunction { std::vector<unsigned char> code; std::uint32_t offset; std::size_t inputs; };
struct TestProgram {
    std::vector<TestFunction> functions;
    std::vector<std::uint32_t> globals;
    static bool read_global(void *context, std::uint32_t index, std::uint32_t &raw) {
        auto &values = static_cast<TestProgram *>(context)->globals;
        if (index >= values.size()) return false;
        raw = values[index]; return true;
    }
    static bool resolve(void *context, std::uint32_t index, pure_vm::FunctionCode &out) {
        auto &program = *static_cast<TestProgram *>(context);
        if (index >= program.functions.size()) return false;
        auto &fn = program.functions[index];
        out = {fn.code.data(), fn.code.size(), fn.inputs, fn.offset, index}; return true;
    }
};
void push(std::vector<unsigned char> &code, std::uint32_t raw) {
    code.push_back(0); code.push_back(4);
    for (int byte = 0; byte < 4; ++byte) code.push_back(static_cast<unsigned char>(raw >> (byte * 8)));
}
void call(TestFunction &fn, std::uint32_t caller, std::uint16_t callee, const std::vector<unsigned char> &arguments = {}) {
    push(fn.code, caller);
    const auto address_at = fn.code.size() + 2; push(fn.code, 0);
    fn.code.insert(fn.code.end(), arguments.begin(), arguments.end());
    fn.code.push_back(12); fn.code.push_back(static_cast<unsigned char>(callee)); fn.code.push_back(static_cast<unsigned char>(callee >> 8));
    const auto address = fn.offset + static_cast<std::uint32_t>(fn.code.size());
    for (int byte = 0; byte < 4; ++byte) fn.code[address_at + byte] = static_cast<unsigned char>(address >> (byte * 8));
}
void nested_tests() {
    TestProgram program{{{{10,4},256,1}, {{10,1},512,1}, {{0,4,5,0,0,0x40,10,0,13},768,0}}};
    call(program.functions[0],0,1,{9,4});
    program.functions[0].code.insert(program.functions[0].code.end(), {9,0,0,4,2,0,0,0x40,16,10,0,13});
    call(program.functions[1],1,2);
    program.functions[1].code.insert(program.functions[1].code.end(), {9,1,9,0,16,10,0,13});
    std::uint32_t result = 0;
    auto run = [&](TestProgram &p, std::uint32_t argument = 0x4000002a) {
        const auto &root = p.functions[0];
        return pure_vm::evaluate(root.code.data(),root.code.size(),&argument,1,result,root.offset,TestProgram::resolve,&p,0,TestProgram::read_global);
    };
    check(run(program) && result == 0x40000031); // 42 + 5 + 2, shared registers across both calls.
    const auto real = [](float n) { return (std::bit_cast<std::uint32_t>(n) >> 2) | 0x80000000; };
    check(run(program,real(1.5f)) && result == real(8.5f));
    auto bad = program; bad.functions[0].code[4] = 1; check(!run(bad)); // Wrong caller index.
    bad = program; bad.functions[0].code[10] ^= 1; check(!run(bad)); // Forged return address.
    bad = program; bad.functions[0].code[17] = 255; check(!run(bad)); // Unknown callee.
    bad = program; bad.functions[1].inputs = 33; check(!run(bad));
    bad = program; bad.functions[1].code = {1,8,13}; check(!run(bad)); // Cannot pop the return packet.
    bad = program; bad.functions[1].code = {2,0xf8,0xff,0xff,0xff,10,0,1,4,13}; check(!run(bad)); // Cannot copy the packet.
    bad = program; bad.functions[2].code = {0x24,13}; check(!run(bad)); // Reachable native/unsupported instruction.
    bad = program; bad.functions[2].code = {0,4,1,0,0,0,10,0,13}; check(!run(bad)); // Control word cannot enter a register.
    bad = program; bad.functions[2].code = {0,4,0,0,0,0xf0,13}; check(!run(bad)); // Cross-program sentinel.
    bad = program; bad.functions[2].code.clear(); call(bad.functions[2],2,2); bad.functions[2].code.push_back(13);
    check(!run(bad)); // Bounded recursion depth, not an engine hang.
    bad = program; bad.functions[2].code.assign(1025,13); check(!run(bad));
    // Total instruction budget includes every callee, not 128 instructions per call.
    bad = program; bad.functions[1].code.clear();
    for (int i = 0; i < 124; ++i) { bad.functions[1].code.push_back(1); bad.functions[1].code.push_back(0); }
    bad.functions[1].code.insert(bad.functions[1].code.end(), {10,0,13});
    check(!run(bad));
    // Nil can be transported through a nested identity call.
    TestProgram identity{{{{10,4},256,1}, {{10,0,13},512,1}}};
    call(identity.functions[0],0,1,{9,4}); identity.functions[0].code.push_back(13);
    check(run(identity,0) && result == 0);
    // A nested leaf sees the same program globals as its caller.
    program.globals = {0x40000005};
    program.functions[2].code = {7,0,0,0,0,10,0,13};
    check(run(program) && result == 0x40000031);
    program.globals[0] = 0x7ffffffe;
    check(run(program) && result == 0x4000002a);
    program.functions[2].code[1] = 1; check(!run(program)); // Out-of-range variable.
    program.functions[2].code[1] = 0;
    for (auto raw : {1u,0xc0000001u,0x9fe00000u}) { program.globals[0] = raw; check(!run(program)); }
    program.functions[2].code = {7,0,0}; check(!run(program)); // Truncated index.
    program.functions[2].code = {8,0,0,0,0,13}; check(!run(program)); // Shared writes remain unsupported.

}
}
int main() {
    try {
        nested_tests();
        std::uint32_t result = 99;
        auto run = [&](std::vector<unsigned char> code, std::vector<std::uint32_t> args = {}) {
            return pure_vm::evaluate(code.data(), code.size(), args.data(), args.size(), result);
        };
        check(run({13}) && result == 0);
        TestProgram globals{{}, {0,0x40000005,0x8fe00000}};
        auto global_run = [&](std::vector<unsigned char> code) {
            return pure_vm::evaluate(code.data(),code.size(),nullptr,0,result,0,nullptr,&globals,0,TestProgram::read_global);
        };
        check(global_run({7,0,0,0,0,10,0,13}) && result == 0);
        check(global_run({7,1,0,0,0,10,0,13}) && result == 0x40000005);
        check(global_run({7,2,0,0,0,10,0,13}) && result == 0x8fe00000);
        check(!global_run({7,3,0,0,0,10,0,13}));
        check(!global_run({7,0xff,0xff,0xff,0xff,10,0,13}));
        check(!run({7,0,0,0,0,10,0,13})); // No global resolver supplied.
        check(!global_run({8,0,0,0,0,13}));
        check(!global_run({7,1,0,0,0,10,0,11,1,0,0,0,13})); // Branch into GLOBAL index.

        check(run({0x26,1,0,0,4,42,0,0,0x40,10,0,13}) && result == 0x4000002a);
        // Supplied argument zero is at the top. Preserve argument one in register 3.
        check(run({10,0,10,3,9,3,10,0,13}, {0x40000001,0x7ffffffe}) && result == 0x7ffffffe);
        check(run({2,0xfc,0xff,0xff,0xff,10,0,1,4,13}, {0x40000005}) && result == 0x40000005);
        check(run({10,0,13}, {0x8fe00000}) && result == 0x8fe00000); // finite float
        check(run({10,0,13}, {0}) && result == 0);
        check(!run({13}, {0x40000001})); // EXIT would pop an argument as return address
        check(!run({1,4,13}));
        check(!run({1,1,13}));
        check(!run({10,0,13}));
        check(!run({9,16,13}));
        check(!run({2,0xfc,0xff,0xff,0xff,13})); // copying the sentinel
        check(!run({2,0,0,0,0,13}));
        check(!run({2,0xff,0xff,0xff,0xff,13}));
        check(!run({0,8,0,0,0,0,13}));
        check(!run({0,4,1,0,0,0,10,0,13})); // reference value
        check(!run({10,0,13}, {0xc0000001})); // string arguments are unsupported
        check(!run({10,0,13}, {0x9fe00000})); // infinity
        check(!run({10,0,13}, {0x9ff00000})); // NaN
        auto encoded = [](std::int64_t n) { return (static_cast<std::uint32_t>(n) & 0x3fffffff) | 0x40000000; };
        auto binary = [&](unsigned op, std::int64_t a, std::int64_t b, std::int64_t expected) {
            check(run({static_cast<unsigned char>(op),10,0,13}, {encoded(b), encoded(a)}) && result == encoded(expected));
        };
        binary(16, -7, 3, -4); binary(17, -7, 3, -10); binary(18, -7, 3, -21);
        binary(19, -7, 3, -2); binary(20, -7, 3, -1);
        for (auto a : {-7,0,3}) for (auto b : {-7,0,3}) {
            binary(21,a,b,a==b); binary(22,a,b,a!=b);
            binary(23,a,b,a>b); binary(24,a,b,a>=b); binary(25,a,b,a<b); binary(26,a,b,a<=b);
            binary(27,a,b,a&b); binary(28,a,b,a|b); binary(29,a,b,a!=0&&b!=0); binary(30,a,b,a!=0||b!=0);
        }
        check(run({31,10,0,13}, {encoded(-7)}) && result == encoded(7));
        check(run({32,10,0,13}, {encoded(0)}) && result == encoded(1));
        check(run({33,10,0,13}, {encoded(-7)}) && result == encoded(6));
        check(!run({19,10,0,13}, {encoded(0),encoded(1)}));
        check(!run({20,10,0,13}, {encoded(0),encoded(1)}));
        check(!run({16,10,0,13}, {encoded(1),encoded(536870911)}));
        check(!run({17,10,0,13}, {encoded(1),encoded(-536870912)}));
        check(!run({18,10,0,13}, {encoded(536870911),encoded(536870911)}));
        check(!run({31,10,0,13}, {encoded(-536870912)}));
        auto vm_float = [](float value) { return (std::bit_cast<std::uint32_t>(value) >> 2) | 0x80000000; };
        check(run({16,10,0,13}, {encoded(1),vm_float(1.5f)}) && result == vm_float(2.5f));
        check(run({17,10,0,13}, {vm_float(1.5f),encoded(-7)}) && result == vm_float(-8.5f));
        check(run({18,10,0,13}, {vm_float(1.5f),encoded(-7)}) && result == vm_float(-10.5f));
        check(run({19,10,0,13}, {encoded(3),vm_float(-7.f)}) && result == vm_float(-7.f/3.f));
        check(run({23,10,0,13}, {encoded(1),vm_float(1.5f)}) && result == encoded(1));
        check(run({24,10,0,13}, {encoded(1),vm_float(1.f)}) && result == encoded(1));
        check(run({25,10,0,13}, {vm_float(1.5f),encoded(1)}) && result == encoded(1));
        check(run({26,10,0,13}, {encoded(1),vm_float(1.f)}) && result == encoded(1));
        check(run({31,10,0,13}, {vm_float(1.5f)}) && result == vm_float(-1.5f));
        for (auto op : {21,22}) {
            check(run({static_cast<unsigned char>(op),10,0,13}, {vm_float(3.f),encoded(3)}) && result == encoded(op == 21));
            check(run({static_cast<unsigned char>(op),10,0,13}, {encoded(3),vm_float(1.5f)}) && result == encoded(op == 22));
            check(run({static_cast<unsigned char>(op),10,0,13}, {encoded(16777217),vm_float(16777216.f)}) && result == encoded(op == 21));
            check(run({static_cast<unsigned char>(op),10,0,13}, {encoded(16777217),encoded(16777216)}) && result == encoded(op == 22));
            check(!run({static_cast<unsigned char>(op),10,0,13}, {0,encoded(0)}));
            check(!run({static_cast<unsigned char>(op),10,0,13}, {0x80000001,encoded(0)}));
            check(!run({0,4,1,0,0,0xc0,0,4,1,0,0,0xc0,static_cast<unsigned char>(op),10,0,13}));
        }
        check(!run({19,10,0,13}, {vm_float(0.f),encoded(1)}));
        check(!run({20,10,0,13}, {vm_float(1.5f),encoded(1)}));
        check(!run({16,10,0,13}, {0x80000001,encoded(1)})); // subnormal
        check(!run({18,10,0,13}, {vm_float(1e30f),vm_float(1e30f)})); // overflow
        check(!run({19,10,0,13}, {vm_float(1e30f),vm_float(1e-30f)})); // underflow/subnormal
        check(!run({16,10,0,13}, {encoded(1),0}));
        for (unsigned op = 0; op <= 0x28; ++op) {
            if (op == 0 || op == 1 || op == 2 || op == 9 || op == 10 || op == 13 || op == 0x26) continue;
            check(!run({static_cast<unsigned char>(op),13}));
        }
        const std::vector<unsigned char> identity{2,0xfc,0xff,0xff,0xff,10,0,1,4,13};
        for (std::size_t size = 0; size < identity.size(); ++size)
            check(!pure_vm::evaluate(identity.data(), size, nullptr, 0, result));
        // Integer condition branches use absolute program offsets and pop the condition.
        std::vector<unsigned char> conditional{14,14,0,0,0,0,4,1,0,0,0x40,10,0,13,0,4,2,0,0,0x40,10,0,13};
        check(run(conditional,{encoded(0)}) && result == encoded(1));
        check(run(conditional,{encoded(-7)}) && result == encoded(2));
        conditional[0] = 15;
        check(run(conditional,{encoded(0)}) && result == encoded(2));
        check(run(conditional,{encoded(-7)}) && result == encoded(1));
        check(!run(conditional,{vm_float(1.f)}));
        conditional[1] = 70;
        const std::uint32_t zero = encoded(0);
        check(pure_vm::evaluate(conditional.data(),conditional.size(),&zero,1,result,56) && result == encoded(2));
        check(!run({11,0,0,0,0,13})); // backward/self jump
        check(!run({11,6,0,0,0,13})); // end/out-of-range jump
        check(run({11,5,0,0,0,13}) && result == 0);
        // A countdown accumulates 1..n and revisits the same bytecode safely.
        const std::vector<unsigned char> loop{10,1,0,4,0,0,0,0x40,10,0,
            9,1,15,40,0,0,0,9,0,9,1,16,10,0,9,1,0,4,1,0,0,0x40,17,10,1,11,10,0,0,0,13};
        for (auto n : {0,1,3,7,11}) check(run(loop,{encoded(n)}) && result == encoded(n*(n+1)/2));
        check(!run(loop,{encoded(12)})); // Requires more than 128 instructions.
        check(!run(loop,{encoded(-1)})); // Nonterminating path never reaches the engine.
        auto absolute_loop = loop; absolute_loop[13] += 56; absolute_loop[36] += 56;
        const auto three = encoded(3);
        check(pure_vm::evaluate(absolute_loop.data(),absolute_loop.size(),&three,1,result,56) && result == encoded(6));
        check(!run({0,4,0,0,0,0x40,10,0,11,1,0,0,0,13})); // Jump into PUSH operand.
        // A forward branch must not let a later backward path reinterpret its operand as code.
        check(!run({11,5,0,0,0,11,2,0,0,0,13}));
        std::vector<unsigned char> overflow;
        for (int i = 0; i < 128; ++i) { overflow.push_back(9); overflow.push_back(0); }
        overflow.push_back(13); check(!run(overflow));
        std::vector<unsigned char> excessive;
        for (int i = 0; i < 128; ++i) { excessive.push_back(1); excessive.push_back(0); }
        excessive.push_back(13); check(!run(excessive));
        check(!run(std::vector<unsigned char>(1025,13)));
        check(!run({13}, std::vector<std::uint32_t>(33)));
        check(!pure_vm::evaluate(identity.data(), identity.size(), nullptr, 1, result));
        result = 123; check(!run({0xff}) && result == 123);
        const std::vector<unsigned char> wait{36,0,28,0,0,4,1,0,0,0x40,36,0,30,1,1,4,36,0,29,0,0,4,42,0,0,0x40,10,0,13};
        private_timer::Preview timer;
        auto native=+[](void *ctx,unsigned group,unsigned command,const std::uint32_t *args,unsigned count){
            return static_cast<private_timer::Preview *>(ctx)->invoke(group,command,args,count);
        };
        check(!pure_vm::evaluate(wait.data(),wait.size(),nullptr,0,result));
        check(pure_vm::evaluate(wait.data(),wait.size(),nullptr,0,result,0,nullptr,&timer,0,nullptr,native) && result==encoded(42) && !timer.active);
        auto malformed=wait; malformed[12]=31; timer={};
        check(!pure_vm::evaluate(malformed.data(),malformed.size(),nullptr,0,result,0,nullptr,&timer,0,nullptr,native));
        malformed=wait; malformed[13]=2; timer={};
        check(!pure_vm::evaluate(malformed.data(),malformed.size(),nullptr,0,result,0,nullptr,&timer,0,nullptr,native));
        malformed=wait; malformed[2]=29; timer={};
        check(!pure_vm::evaluate(malformed.data(),malformed.size(),nullptr,0,result,0,nullptr,&timer,0,nullptr,native));
        malformed=wait; const auto negative=encoded(-1); std::memcpy(malformed.data()+6,&negative,4); timer={};
        check(!pure_vm::evaluate(malformed.data(),malformed.size(),nullptr,0,result,0,nullptr,&timer,0,nullptr,native));
        malformed=wait; const auto long_wait=encoded(31); std::memcpy(malformed.data()+6,&long_wait,4); timer={};
        check(!pure_vm::evaluate(malformed.data(),malformed.size(),nullptr,0,result,0,nullptr,&timer,0,nullptr,native));
        auto read_timer=+[](void *ctx,unsigned group,unsigned command,const std::uint32_t *args,unsigned count,std::uint32_t &raw) {
            return static_cast<private_timer::Preview *>(ctx)->read_result(group,command,args,count,raw);
        };
        const std::vector<unsigned char> timer_read{36,0,31,0,9,0,0,4,0,0,0xc0,0x8f,16,10,0,13};
        timer={};
        check(pure_vm::evaluate(timer_read.data(),timer_read.size(),nullptr,0,result,0,nullptr,&timer,0,nullptr,nullptr,read_timer) && result==0x8fc00000u && timer.read);
        timer={};auto wrong_read=timer_read;wrong_read[2]=32;
        check(!pure_vm::evaluate(wrong_read.data(),wrong_read.size(),nullptr,0,result,0,nullptr,&timer,0,nullptr,nullptr,read_timer));
        auto mixed=wait;mixed.insert(mixed.end()-1,{36,0,31,0});timer={};
        check(!pure_vm::evaluate(mixed.data(),mixed.size(),nullptr,0,result,0,nullptr,&timer,0,nullptr,native,read_timer));
        auto invalid_result=+[](void *,unsigned,unsigned,const std::uint32_t *,unsigned,std::uint32_t &raw) {raw=0x9fe00000u;return true;};
        check(!pure_vm::evaluate(timer_read.data(),timer_read.size(),nullptr,0,result,0,nullptr,nullptr,0,nullptr,nullptr,invalid_result));
        std::cout << "Pure VM stack and rejection checks passed\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
