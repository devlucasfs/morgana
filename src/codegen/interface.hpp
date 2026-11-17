#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>

#include "../recognize/parser.hpp"

/* Helper: std::string -> const char*, usable inside printf-style calls. */
inline const char *c(const std::string &s) {
    return s.c_str();
}

#define repeat3(x) (x), (x), (x)
#define repeat2(x) (x), (x)

class codegen_interface_methods {
protected:
    FILE *output;

public:
    bool into = false;
    std::string entrypoint;

    explicit codegen_interface_methods(FILE *output, std::string entrypoint)
        : output(output), entrypoint(entrypoint) {}

    virtual ~codegen_interface_methods() = default;

    /* Writes fmt (printf-style) to `output`.
     * If `into` is true, every line of fmt gets a '\t' prefix.
     * Empty lines stay empty (no trailing tabs).
     * Note: format index starts at 2 because `this` is argument 1. */
    __attribute__((format(printf, 2, 3)))
    void emit(const char *fmt, ...) {
        va_list ap;
        va_start(ap, fmt);

        if(! into ) {
            vfprintf(output, fmt, ap);
            va_end(ap);
            return;
        }

        std::string indented;
        indented.reserve(std::char_traits<char>::length(fmt) * 2 + 1);

        bool bol = true;
        for( const char *s = fmt; *s; s++ ) {
            if( bol && *s != '\n' ) indented += '\t';
            indented += *s;
            bol = (*s == '\n');
        }

        vfprintf(output, indented.c_str(), ap);
        va_end(ap);
    }

    /* functions */
    virtual void prologue(prologue_t prologue) = 0;
    virtual void epilogue(epilogue_t epilogue) = 0;
    virtual void extern_op(extern_op_t op) = 0;
    virtual void unfold_op(unfold_op_t op) = 0;
    virtual void destroyer_op(destroyer_op_t op) = 0;

    /* memory */
    virtual void alloc_op(alloc_op_t op) = 0;
    virtual void ptr_to_op(ptr_to_op_t op) = 0;
    virtual void move_op(move_op_t op) = 0;
    virtual void load_op(load_op_t op) = 0;
    virtual void pack_op(pack_op_t op) = 0;
    virtual void deref_op(deref_op_t op) = 0;
    virtual void store_op(store_op_t op) = 0;
    virtual void storep_op(storep_op_t op) = 0;
    virtual void str_op(str_op_t op) = 0;

    /* calls */
    virtual void call_op(call_op_t op) = 0;
    virtual void call_indirect_op(call_indirect_op_t op) = 0;
    virtual void ret_op(ret_op_t op) = 0;
    virtual void syscall_op(syscall_op_t op) = 0;

    /* arithmetic and control flow */
    virtual void binary_op(binary_op_t op) = 0;
    virtual void cast_op(cast_op_t op) = 0;
    virtual void label_op(label_op_t op) = 0;
    virtual void br_op(br_op_t op) = 0;
    virtual void branch_op(branch_op_t op) = 0;

    /* registers and mapped io */
    virtual void loc_load_op(loc_load_op_t op) = 0;
    virtual void loc_store_op(loc_store_op_t op) = 0;
    virtual void loc_get_op(loc_get_op_t op) = 0;
    virtual void loc_set_op(loc_set_op_t op) = 0;
};
