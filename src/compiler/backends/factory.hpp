#pragma once
#include <string>

struct syscall_arg {
    bool        lea  = false;
    bool        imm  = false;
    std::string src;
    std::size_t size = 8;
};

#include "../codegen/backends.hpp"
#include "x86_64_gnu.hpp"
// #include "./backends/x86_64_sysv.hpp"
// #include "./backends/x86_64_windows.hpp"
// #include "./backends/aarch64.hpp"

#include <memory>
#include <stdexcept>


struct assembly_factory {
    static std::unique_ptr<assembly_backend> create(const target& config) {
        if( config.architecture == X86_64 && config.convention == GNU ) return std::make_unique<x86_64_gnu>(
            config.output
        );

        throw std::runtime_error(
            "Unsupported architecture and ABI combination"
        );
    }
};
