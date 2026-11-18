#pragma once

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

#include "exports.hpp"
#include "../hash.hpp"
#include "../recognize/parser.hpp"
#include "./interface.hpp"
#include "./optimizer.hpp"

std::unique_ptr<codegen_interface_methods> instance(
    std::string target,
    FILE *output
) {
    #define C(name, class_name, _, entry) \
    case const_hash(name): return std::make_unique<class_name>(output, entry);

    switch(const_hash(target.c_str())) {
        EXTENSORS(C)
        default: throw std::runtime_error(target + " isnt a valid extensor");
    }
    #undef C
}

std::unique_ptr<behaviour_interface_methods> behaviours(
    std::string target
) {
    #define B(name, _, class_name, entry) \
    case const_hash(name): return std::make_unique<class_name>();

    switch(const_hash(target.c_str())) {
        EXTENSORS(B)
        default: throw std::runtime_error(target + " isnt a valid extensor");
    }
    #undef B
}

#define check_it(T, block)                                       \
if( std::holds_alternative<T>(it) ) do {                         \
    auto data = std::get<T>(it);                                 \
    block                                                        \
    continue;                                                    \
} while(false)

#define inline_it(T)                                            \
check_it(T, { backend->T(data); });

void codegen(std::string target, FILE *output, Ins ins) {
    auto backend = instance(target, output);
    auto nis = optimize(ins, opt_options {
        .roots = { backend->entrypoint },
        .prune_declarations = true,
    });

    for( auto& it : nis.set ) {
        inline_it(prologue);
        inline_it(epilogue);
        inline_it(extern_op);
        inline_it(unfold_op);
        inline_it(destroyer_op);

        inline_it(alloc_op);
        inline_it(ptr_to_op);
        inline_it(move_op);
        inline_it(load_op);
        inline_it(pack_op);
        inline_it(deref_op);
        inline_it(store_op);
        inline_it(storep_op);
        inline_it(str_op);

        inline_it(call_op);
        inline_it(call_indirect_op);
        inline_it(ret_op);
        inline_it(syscall_op);

        inline_it(binary_op);
        inline_it(cast_op);
        inline_it(label_op);
        inline_it(br_op);
        inline_it(branch_op);

        inline_it(loc_load_op);
        inline_it(loc_store_op);
        inline_it(loc_get_op);
        inline_it(loc_set_op);
    }
}
