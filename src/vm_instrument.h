#pragma once
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace budget_vm {
struct Image {
    std::vector<unsigned char> data;
    std::map<std::uint32_t, std::uint32_t> addresses;
    std::size_t functions = 0;
};
// Append instrumented code; original metadata, defaults and string offsets stay put.
// The private thread must supply group 28's checkpoint dispatcher before Start.
inline bool build(std::span<const unsigned char> source, std::uint32_t root, Image &output, std::string &error) {
    auto fail = [&](const char *reason) { error = reason; return false; };
    if (source.size() < 24 || source.size() > 16 * 1024 * 1024 || std::memcmp(source.data(), "#scp", 4))
        return fail("invalid compiled program");
    auto region = [&](std::uint32_t at, std::uint64_t size) { return at <= source.size() && size <= source.size() - at; };
    auto word = [&](std::uint32_t at) { std::uint32_t raw; std::memcpy(&raw, source.data() + at, 4); return raw; };
    const auto table = word(4), count = word(8), variables = word(12);
    const auto variable_count = static_cast<std::uint64_t>(word(16)) + word(20);
    if (table < 24 || !count || count > 65536 || root >= count || !region(table, count * 32ull) ||
        variable_count > 4096 || !region(variables, variable_count * 8)) return fail("invalid compiled metadata");
    struct Range { std::uint32_t start, end; };
    std::vector<Range> metadata{{0,24},{table,table + count * 32}};
    auto exclude = [&](std::uint32_t start, std::uint32_t size) { if (size) metadata.push_back({start,start + size}); };
    exclude(variables, static_cast<std::uint32_t>(variable_count * 8));
    auto string = [&](std::uint32_t tagged) {
        if ((tagged >> 30) != 3) return false;
        const auto offset = tagged & 0x3fffffff;
        if (!offset || !region(offset, 1)) return false;
        const auto size = std::min<std::size_t>(16384, source.size() - offset);
        const auto end = static_cast<const unsigned char *>(std::memchr(source.data() + offset, 0, size));
        if (!end) return false;
        exclude(offset, static_cast<std::uint32_t>(end - source.data() - offset + 1)); return true;
    };
    struct Function { std::uint32_t code, limit; unsigned char inputs, flags0, flags1, defaults; };
    std::vector<Function> functions;
    std::set<std::uint32_t> entries;
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto at = table + index * 32, code = word(at), input_table = word(at + 12), default_table = word(at + 8);
        const auto extra_count = word(at + 16), extra_table = word(at + 20);
        const auto inputs = source[at + 4], defaults = source[at + 7];
        if (!region(code, 1) || defaults > inputs || !region(input_table, inputs * 4ull) ||
            !region(default_table, defaults * 4ull) || extra_count > 65536 || !region(extra_table, extra_count * 12ull) ||
            !string(word(at + 28))) return fail("invalid function metadata");
        exclude(input_table, inputs * 4); exclude(default_table, defaults * 4); exclude(extra_table, extra_count * 12);
        functions.push_back({code,0,inputs,source[at + 5],source[at + 6],defaults}); entries.insert(code);
    }
    for (std::uint32_t index = 0; index < variable_count; ++index)
        if (!string(word(variables + index * 8))) return fail("invalid variable metadata");
    for (auto &function : functions) {
        const auto next = entries.upper_bound(function.code);
        function.limit = next == entries.end() ? static_cast<std::uint32_t>(source.size()) : *next;
    }
    struct Instruction { std::uint32_t address, width, function; unsigned char op; };
    std::map<std::uint32_t, Instruction> instructions;
    std::set<std::uint32_t> linked;
    std::vector<std::uint32_t> pending_functions{root};
    while (!pending_functions.empty()) {
        const auto index = pending_functions.back(); pending_functions.pop_back();
        if (!linked.insert(index).second) continue;
        if (linked.size() > 256) return fail("call graph exceeds 256 functions");
        const auto &function = functions[index];
        if (function.flags0 > 1 || function.flags1) return fail("unsupported function flags");
        std::vector<std::uint32_t> pending{function.code};
        std::set<std::uint32_t> visited;
        while (!pending.empty()) {
            const auto address = pending.back(); pending.pop_back();
            if (!visited.insert(address).second) continue;
            if (address < function.code || address >= function.limit) return fail("branch leaves its function");
            const auto op = source[address];
            if (op > 40) return fail("unknown VM opcode");
            if (op == 34 || op == 35 || op == 37) return fail("external script calls require linked program instrumentation");
            const std::uint32_t width = op == 0 ? 6 : op == 12 || op == 38 ? 3 : op == 36 ? 4 :
                op == 1 || op == 9 || op == 10 || op == 39 ? 2 :
                (op >= 2 && op <= 8) || op == 11 || op == 14 || op == 15 || op == 40 ? 5 : 1;
            if (width > function.limit - address) return fail("truncated VM instruction");
            if ((op == 0 && source[address + 1] != 4) || ((op == 9 || op == 10) && source[address + 1] >= 16) ||
                (op == 1 && source[address + 1] % 4) || (op == 36 && source[address + 1] >= 28))
                return fail("invalid VM operand");
            if (op == 0 && (word(address + 2) >> 30) == 3 && !string(word(address + 2))) return fail("invalid VM string");
            const auto next = instructions.lower_bound(address);
            if (next != instructions.end() && next->first == address) {
                if (next->second.width != width) return fail("overlapping VM instructions");
            } else {
                if ((next != instructions.end() && next->first < address + width) ||
                    (next != instructions.begin() && std::prev(next)->first + std::prev(next)->second.width > address))
                    return fail("overlapping VM instructions");
                if (instructions.size() == 65536) return fail("call graph exceeds 65536 instructions");
                instructions.emplace(address, Instruction{address,width,index,op});
            }
            if (op == 12) {
                std::uint16_t callee; std::memcpy(&callee, source.data() + address + 1, 2);
                if (callee >= count) return fail("invalid callee index");
                pending_functions.push_back(callee);
            }
            if (op == 11 || op == 14 || op == 15) pending.push_back(word(address + 1));
            if (op != 11 && op != 13) pending.push_back(address + width);
        }
    }
    std::sort(metadata.begin(), metadata.end(), [](const auto &a, const auto &b) { return a.start < b.start; });
    std::vector<Range> merged;
    for (const auto &range : metadata) {
        if (!merged.empty() && range.start <= merged.back().end) merged.back().end = std::max(range.end,merged.back().end);
        else merged.push_back(range);
    }
    for (const auto &[address,instruction] : instructions) {
        const auto next = std::lower_bound(merged.begin(),merged.end(),address + instruction.width,
            [](const Range &range, std::uint32_t end) { return range.start < end; });
        if (next != merged.begin() && std::prev(next)->end > address) return fail("VM code overlaps program metadata");
    }
    // CALL is a tail jump. Its compiled packet pushes caller index then the exact
    // original return address before evaluating arguments. Relocate that word only.
    std::map<std::uint32_t,std::uint32_t> return_words;
    std::set<std::uint32_t> caller_words;
    std::map<std::pair<std::uint32_t,std::uint32_t>,std::vector<std::uint32_t>> packets;
    for (auto current = instructions.begin(); current != instructions.end(); ++current) {
        if (current->second.op != 0 || current == instructions.begin()) continue;
        const auto previous = std::prev(current);
        if (previous->second.op != 0 || previous->first + 6 != current->first) continue;
        const auto caller = word(previous->first + 2), returning = word(current->first + 2);
        if (caller < count && returning && returning < 0x0fffffff &&
            functions[caller].code == functions[current->second.function].code)
            packets[{returning,functions[caller].code}].push_back(current->first);
    }
    for (const auto &[address,instruction] : instructions) if (instruction.op == 12) {
        const auto returning = address + 3;
        const auto found = packets.find({returning,functions[instruction.function].code});
        if (found == packets.end() || found->second.size() != 1) return fail("compiled CALL packet is missing or ambiguous");
        const auto packet = found->second.front();
        return_words[packet] = returning; caller_words.insert(packet - 6);
    }
    for (const auto &[address,instruction] : instructions) if (instruction.op == 0) {
        const auto raw = word(address + 2);
        if (raw && !(raw >> 30) && !return_words.contains(address) && !caller_words.contains(address))
            return fail("unrecognized raw control word");
    }
    Image candidate; candidate.data.assign(source.begin(),source.end()); candidate.functions = linked.size();
    for (const auto &[address,instruction] : instructions) {
        if (candidate.data.size() + 4 + instruction.width >= 0x0fffffff) return fail("instrumented code exceeds VM address limit");
        candidate.addresses[address] = static_cast<std::uint32_t>(candidate.data.size());
        candidate.data.insert(candidate.data.end(), {36,28,0,0});
        candidate.data.insert(candidate.data.end(),source.begin() + address,source.begin() + address + instruction.width);
    }
    auto patch = [&](std::uint32_t at, std::uint32_t value) { std::memcpy(candidate.data.data() + at,&value,4); };
    for (auto index : linked) patch(table + index * 32,candidate.addresses.at(functions[index].code));
    for (const auto &[address,instruction] : instructions) {
        if (instruction.op == 11 || instruction.op == 14 || instruction.op == 15)
            patch(candidate.addresses.at(address) + 5,candidate.addresses.at(word(address + 1)));
    }
    for (const auto &[address,returning] : return_words) patch(candidate.addresses.at(address) + 6,candidate.addresses.at(returning));
    output = std::move(candidate); error.clear(); return true;
}
}
