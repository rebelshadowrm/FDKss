#include "vm_instrument.h"
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string_view>

int main(int argc, char **argv) {
    try {
        if (argc != 3 && argc != 4) throw std::runtime_error("usage: fdkss_vm_instrument SOURCE.scp FUNCTION_INDEX [OUTPUT.scp]");
        std::uint32_t root = 0;
        const std::string_view index = argv[2];
        const auto parsed = std::from_chars(index.data(),index.data() + index.size(),root);
        if (parsed.ec != std::errc{} || parsed.ptr != index.data() + index.size()) throw std::runtime_error("function index must be an unsigned integer");
        const std::filesystem::path source = argv[1];
        const auto size = std::filesystem::file_size(source);
        if (size < 24 || size > 16 * 1024 * 1024) throw std::runtime_error("invalid compiled program size");
        if (argc == 4 && std::filesystem::exists(argv[3]) && std::filesystem::equivalent(source,argv[3]))
            throw std::runtime_error("output must differ from the source file");
        std::ifstream stream(source,std::ios::binary);
        std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
        if (!stream.read(reinterpret_cast<char *>(bytes.data()),static_cast<std::streamsize>(size))) throw std::runtime_error("could not read compiled program");
        budget_vm::Image image; std::string error;
        if (!budget_vm::build(bytes,root,image,error)) throw std::runtime_error(error);
        if (argc == 4) {
            std::ofstream output(argv[3],std::ios::binary | std::ios::trunc);
            if (!output.write(reinterpret_cast<const char *>(image.data.data()),static_cast<std::streamsize>(image.data.size())))
                throw std::runtime_error("could not write instrumented program");
        }
        std::cout << "{\"format\":\"fdkss.vm.budget.preview\",\"root_index\":" << root << ",\"function_count\":" << image.functions
            << ",\"instruction_count\":" << image.addresses.size() << ",\"size_bytes\":" << image.data.size()
            << ",\"checkpoint_group\":28,\"requires_private_dispatcher\":true}\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
