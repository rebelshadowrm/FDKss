#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pure_vm {
inline std::int64_t integer(std::uint32_t raw) {
    const auto bits = raw & 0x3fffffff;
    return (bits & 0x20000000) ? static_cast<std::int64_t>(bits) - 0x40000000 : bits;
}
inline bool integer_operation(unsigned op, std::uint32_t left, std::uint32_t right, std::uint32_t &out) {
    if ((left >> 30) != 1 || (right >> 30) != 1) return false;
    const auto a = integer(left), b = integer(right);
    std::int64_t value = 0;
    switch (op) {
    case 16: value = a + b; break;
    case 17: value = a - b; break;
    case 18: value = a * b; break;
    case 19: if (!b) return false; value = a / b; break;
    case 20: if (!b) return false; value = a % b; break;
    case 21: value = a == b; break;
    case 22: value = a != b; break;
    case 23: value = a > b; break;
    case 24: value = a >= b; break;
    case 25: value = a < b; break;
    case 26: value = a <= b; break;
    case 27: value = a & b; break;
    case 28: value = a | b; break;
    case 29: value = a != 0 && b != 0; break;
    case 30: value = a != 0 || b != 0; break;
    case 31: value = -b; break;
    case 32: value = b == 0; break;
    case 33: value = ~b; break;
    default: return false;
    }
    // Deliberately reject arithmetic overflow instead of relying on VM wrapping.
    if (value < -536870912 || value > 536870911) return false;
    out = (static_cast<std::uint32_t>(value) & 0x3fffffff) | 0x40000000;
    return true;
}
inline bool numeric_operation(unsigned op, std::uint32_t left, std::uint32_t right, std::uint32_t &out) {
    if ((left >> 30) == 1 && (right >> 30) == 1) return integer_operation(op, left, right, out);
    if (((left >> 30) != 1 && (left >> 30) != 2) || ((right >> 30) != 1 && (right >> 30) != 2)) return false;
    const float a = (left >> 30) == 1 ? static_cast<float>(integer(left)) : std::bit_cast<float>(left << 2);
    const float b = (right >> 30) == 1 ? static_cast<float>(integer(right)) : std::bit_cast<float>(right << 2);
    if (!std::isfinite(a) || !std::isfinite(b) || std::fpclassify(a) == FP_SUBNORMAL || std::fpclassify(b) == FP_SUBNORMAL) return false;
    float value = 0;
    switch (op) {
    case 16: value = a + b; break;
    case 17: value = a - b; break;
    case 18: value = a * b; break;
    case 19: if (b == 0) return false; value = a / b; break;
    case 21: out = 0x40000000u | static_cast<std::uint32_t>(a == b); return true;
    case 22: out = 0x40000000u | static_cast<std::uint32_t>(a != b); return true;
    case 23: out = 0x40000000u | static_cast<std::uint32_t>(a > b); return true;
    case 24: out = 0x40000000u | static_cast<std::uint32_t>(a >= b); return true;
    case 25: out = 0x40000000u | static_cast<std::uint32_t>(a < b); return true;
    case 26: out = 0x40000000u | static_cast<std::uint32_t>(a <= b); return true;
    case 31: value = -b; break;
    default: return false;
    }
    if (!std::isfinite(value) || std::fpclassify(value) == FP_SUBNORMAL ||
        (value == 0 && ((op == 18 && a != 0 && b != 0) || (op == 19 && a != 0) ||
            (op == 16 && a != -b) || (op == 17 && a != b)))) return false;
    out = (std::bit_cast<std::uint32_t>(value) >> 2) | 0x80000000;
    return true;
}
inline bool scalar(std::uint32_t raw) {
    return raw == 0 || (raw >> 30) == 1 || (raw >> 30) == 3 ||
        ((raw >> 30) == 2 && std::isfinite(std::bit_cast<float>(raw << 2)));
}
struct FunctionCode {
    const unsigned char *code = nullptr;
    std::size_t size = 0, inputs = 0;
    std::uint32_t offset = 0, index = 0;
};
using Resolve = bool (*)(void *, std::uint32_t, FunctionCode &);
using ReadGlobal = bool (*)(void *, std::uint32_t, std::uint32_t &);
using NativePreview = bool (*)(void *, unsigned, unsigned, const std::uint32_t *, unsigned);
using NativeResultPreview = bool (*)(void *, unsigned, unsigned, const std::uint32_t *, unsigned, std::uint32_t &);
// Preflight only: the real FDK interpreter still performs the verified call.
// Accept only paths that finish within 128 instructions, including bounded loops.
// Native instructions are rejected unless an explicit preview policy accepts
// them. A separate result policy may predict a validated numeric/nil register-zero
// effect. Policies must exclude shared writes and unpredictable context.
inline bool evaluate(const unsigned char *code, std::size_t size, const std::uint32_t *args,
    std::size_t count, std::uint32_t &result, std::uint32_t code_offset = 0,
    Resolve resolve = nullptr, void *context = nullptr, std::uint32_t function_index = 0,
    ReadGlobal read_global = nullptr, NativePreview native = nullptr, NativeResultPreview native_result = nullptr) {
    if (!code || !size || size > 1024 || count > 32 || (count && !args)) return false;
    std::array<std::uint32_t, 128> stack{};
    std::array<bool, 128> control{};
    std::array<std::uint32_t, 16> registers{};
    struct Instruction { std::uint64_t start, end; };
    std::array<Instruction, 128> instructions{};
    std::size_t instruction_count = 0;
    struct Frame { FunctionCode caller; std::size_t floor; std::uint32_t address; };
    std::array<Frame, 8> frames{};
    std::size_t depth = 0, floor = 1;
    stack[0] = 0x0fffffff; // Start's private outermost return sentinel.
    std::size_t used = 1, offset = 0;
    for (std::size_t index = count; index > 0; --index) {
        if (!scalar(args[index - 1]) || (args[index - 1] >> 30) == 3) return false;
        stack[used++] = args[index - 1];
    }
    for (unsigned step = 0; step < 128 && offset < size; ++step) {
        const auto start = offset;
        const auto instruction = code[start];
        const std::size_t width = instruction == 0 ? 6 : instruction == 36 ? 4 : instruction == 0x26 ? 3 :
            instruction == 12 ? 3 :
            (instruction == 1 || instruction == 9 || instruction == 10) ? 2 :
            (instruction == 2 || instruction == 7 || instruction == 11 || instruction == 14 || instruction == 15) ? 5 : 1;
        if (width > size - start) return false;
        const auto absolute = static_cast<std::uint64_t>(code_offset) + start;
        bool visited = false;
        for (std::size_t index = 0; index < instruction_count; ++index) {
            const auto &previous = instructions[index];
            if (previous.start == absolute && previous.end == absolute + width) visited = true;
            else if (absolute < previous.end && previous.start < absolute + width) return false;
        }
        if (!visited) instructions[instruction_count++] = {absolute, absolute + width};
        const auto op = code[offset++];
        if (op == 0x26) { if (size - offset < 2) return false; offset += 2; }
        else if (op == 0) {
            if (size - offset < 5 || code[offset++] != 4 || used == stack.size()) return false;
            std::uint32_t raw = 0; std::memcpy(&raw, code + offset, 4); offset += 4;
            const bool address = raw && (raw >> 30) == 0;
            if ((!scalar(raw) && !(resolve && address && raw < 0x0fffffff)) || raw == 0xf0000000) return false;
            control[used] = address; stack[used++] = raw;
        } else if (op == 1) {
            if (offset == size) return false;
            const auto bytes = code[offset++];
            if (bytes % 4 || bytes / 4 > used - floor) return false;
            used -= bytes / 4;
        } else if (op == 2) {
            if (size - offset < 4 || used == stack.size()) return false;
            std::int32_t relative = 0; std::memcpy(&relative, code + offset, 4); offset += 4;
            const auto source = static_cast<std::int64_t>(used) * 4 + relative;
            if (relative % 4 || source < static_cast<std::int64_t>(floor) * 4 || source >= static_cast<std::int64_t>(used) * 4 ||
                control[static_cast<std::size_t>(source) / 4]) return false;
            const auto raw = stack[static_cast<std::size_t>(source) / 4];
            control[used] = false; stack[used++] = raw;
        } else if (op == 7) {
            if (!read_global || size - offset < 4 || used == stack.size()) return false;
            std::uint32_t index = 0, raw = 0; std::memcpy(&index, code + offset, 4); offset += 4;
            if (!read_global(context, index, raw) || !scalar(raw) || (raw >> 30) == 3) return false;
            control[used] = false; stack[used++] = raw;
        } else if (op == 9 || op == 10) {
            if (offset == size || code[offset] >= registers.size()) return false;
            const auto reg = code[offset++];
            if (op == 9) { if (used == stack.size()) return false; control[used] = false; stack[used++] = registers[reg]; }
            else { if (used <= floor || control[used - 1]) return false; registers[reg] = stack[--used]; }
        } else if (op == 11 || op == 14 || op == 15) {
            if (size - offset < 4) return false;
            std::uint32_t target = 0; std::memcpy(&target, code + offset, 4); offset += 4;
            if (target < code_offset || target - code_offset >= size) return false;
            for (std::size_t index = 0; index < instruction_count; ++index)
                if (target > instructions[index].start && target < instructions[index].end) return false;
            bool take = true;
            if (op != 11) {
                if (used <= floor || (stack[used - 1] >> 30) != 1) return false;
                const auto condition = integer(stack[--used]);
                take = op == 14 ? condition != 0 : condition == 0;
            }
            if (take) offset = target - code_offset;
        } else if (op == 12) {
            if (!resolve || depth == frames.size() || size - offset < 2) return false;
            std::uint16_t target = 0; std::memcpy(&target, code + offset, 2); offset += 2;
            FunctionCode callee{};
            if (!resolve(context, target, callee) || !callee.code || !callee.size || callee.size > 1024 ||
                callee.index != target || callee.inputs > 32 || used < floor + callee.inputs + 2 || offset >= size ||
                static_cast<std::uint64_t>(code_offset) + offset >= 0x0fffffff) return false;
            const auto packet = used - callee.inputs - 2;
            const auto address = code_offset + static_cast<std::uint32_t>(offset);
            if (stack[packet] != function_index || stack[packet + 1] != address) return false;
            for (std::size_t index = packet + 2; index < used; ++index)
                if (control[index] || !scalar(stack[index])) return false;
            frames[depth++] = {{code, size, count, code_offset, function_index}, floor, address};
            floor = packet + 2;
            code = callee.code; size = callee.size; count = callee.inputs;
            code_offset = callee.offset; function_index = callee.index; offset = 0;
        } else if (op >= 16 && op <= 33) {
            const bool unary = op >= 31;
            if (used < floor + (unary ? 1u : 2u)) return false;
            const auto right = stack[--used];
            const auto left = unary ? 0x40000000u : stack[--used];
            std::uint32_t raw = 0;
            if (!numeric_operation(op, left, right, raw)) return false;
            control[used] = false; stack[used++] = raw;
        } else if (op == 36) {
            if ((!native && !native_result) || size-offset<3) return false;
            const auto group=code[offset++], command=code[offset++], argc=code[offset++];
            if (argc>used-floor) return false;
            for (std::size_t i=used-argc;i<used;++i) if (control[i] || !scalar(stack[i])) return false;
            auto predicted=registers[0];
            if (native_result && native_result(context,group,command,stack.data()+used-argc,argc,predicted)) {
                if (!scalar(predicted) || (predicted>>30)==3) return false;
                registers[0]=predicted;
            } else if (!native || !native(context,group,command,stack.data()+used-argc,argc)) return false;
        } else if (op == 13) {
            if (depth) {
                const auto &frame = frames[--depth];
                if (used != floor || used < 2 || stack[used - 1] != frame.address || stack[used - 2] != frame.caller.index) return false;
                used -= 2; floor = frame.floor;
                code = frame.caller.code; size = frame.caller.size; count = frame.caller.inputs;
                code_offset = frame.caller.offset; function_index = frame.caller.index;
                offset = frame.address - code_offset;
                continue;
            }
            if (used != 1 || stack[0] != 0x0fffffff || !scalar(registers[0])) return false;
            result = registers[0]; return true;
        } else return false;
    }
    return false;
}
}
