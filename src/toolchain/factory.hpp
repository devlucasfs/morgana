#pragma once

#include "./behaviours.hpp"
#include "../codegen/backends.hpp"
#include "../../export.hpp"

#include <memory>

struct behaviour_factory {
    static std::unique_ptr<assembly_behaviour> create(const target_t& config) {
        if( config.architecture == AVRATMEGA328P && config.convention == NOS ) return std::make_unique<build_avr_atmega328p>(config);
        if( config.architecture == X86_64 && config.convention == GNU ) return std::make_unique<build_x86_64_gnu>(config);

        throw std::runtime_error(
            "Unsupported architecture and ABI combination"
        );
    }
};
