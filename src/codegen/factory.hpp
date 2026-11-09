#pragma once
#include <string>

struct syscall_arg {
    bool        lea  = false;
    bool        imm  = false;
    std::string src;
    std::size_t size = 8;
};

#include "./backends.hpp"
#include "export.hpp"
#include <memory>
#include <stdexcept>

struct assembly_factory {
    static std::unique_ptr<assembly_backend> create(const target_t& config) {
        if( config.architecture == AVRATMEGA328P && config.convention == NOS ) return std::make_unique<avr_atmega328p>(
            config.output
        );

        if( config.architecture == X86_64 && config.convention == GNU ) return std::make_unique<x86_64_gnu>(
            config.output
        );

        throw std::runtime_error(
            "Unsupported architecture and ABI combination"
        );
    }
};
