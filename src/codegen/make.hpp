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
check_it(T, { backend->T(data); });

void codegen(std::string target, FILE *output, Ins ins) {
    auto backend = instance(target, output);
    for( auto& it : ins.set ) {
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
