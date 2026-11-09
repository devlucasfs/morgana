#pragma once

#include "./codegen/backends.hpp"
#include "./compiler/parser.hpp"
#include <string>

void gen_target(target_t *config, std::string arch_os) {
    switch( hash(arch_os) ) {
               case hash("avr_atmega328p"): *config = { .architecture = AVRATMEGA328P, .convention = NOS };
        break; case hash("x86_64-linux"):   *config = { .architecture = X86_64,        .convention = GNU };
        break; default: CompilerOutputs::fatal("Unknown arch");
    }
}
