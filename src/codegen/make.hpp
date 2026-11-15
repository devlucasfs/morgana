#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

#include "exports.hpp"
#include "../recognize/parser.hpp"
#include "./interface.hpp"

constexpr unsigned const_hash(const char *input) {
    return (*input)
        ? static_cast<unsigned>(*input) + 33 * const_hash(input + 1)
        : 5381;
}

std::unique_ptr<codegen_interface_methods> instance(
    std::string target,
    FILE *output
) {
    switch(const_hash(target.c_str())) {
        case const_hash("x86_64-gnu"):
            return std::make_unique<x86_64_gnu>(output);

        default:
            throw std::runtime_error(target + " isnt a valid extensor");
    }
}

#define check_it(T, block)                                       \
if( std::holds_alternative<T>(it) ) do {                         \
    auto data = std::get<T>(it);                                 \
    block                                                        \
    continue;                                                    \
} while(false)

#define inline_it(T)                                            \
check_it(T, { backend->T(data); });                              \

void codegen(std::string target, FILE *output, Ins ins) {
    auto backend = instance(target, output);
    for( auto& it : ins.set ) {
        inline_it(prologue);
        inline_it(epilogue);
    }
}
