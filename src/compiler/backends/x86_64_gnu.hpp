#pragma once

#include "../codegen/backends.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>

class x86_64_gnu : public assembly_backend {
protected:
    static constexpr std::size_t arg_register_count = 6;

    std::size_t stack_args = 0;
    std::size_t sret_slot = 0;
    std::size_t pending_cleanup = 0;

    static std::string generate_position(std::size_t pos) {
        return "-" + std::to_string(pos) + "(%rbp)";
    }

    static const char* accumulator(std::size_t size) {
        switch( size ) {
            case 1: return "%al";
            case 2: return "%ax";
            case 4: return "%eax";
            case 8: return "%rax";
            default:
                throw std::runtime_error(
                    "Unsupported operand size: " + std::to_string(size)
                );
        }
    }

    static const char* move_instruction(std::size_t size) {
        switch( size ) {
            case 1: return "movb";
            case 2: return "movw";
            case 4: return "movl";
            case 8: return "movq";
            default:
                throw std::runtime_error(
                    "Unsupported operand size: " + std::to_string(size)
                );
        }
    }

public:
    explicit x86_64_gnu(const std::string& output) : assembly_backend(output) {}

    inline static constexpr const char* registers[arg_register_count] = {
        "%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"
    };

    static const char* param_register(std::size_t index, std::size_t size) {
        static constexpr const char* table[arg_register_count][4] = {
            { "%dil", "%di",  "%edi",  "%rdi" },
            { "%sil", "%si",  "%esi",  "%rsi" },
            { "%dl",  "%dx",  "%edx",  "%rdx" },
            { "%cl",  "%cx",  "%ecx",  "%rcx" },
            { "%r8b", "%r8w", "%r8d",  "%r8"  },
            { "%r9b", "%r9w", "%r9d",  "%r9"  },
        };

        switch( size ) {
            case 1: return table[index][0];
            case 2: return table[index][1];
            case 4: return table[index][2];
            case 8: return table[index][3];
            default:
                throw std::runtime_error(
                    "Unsupported operand size: " + std::to_string(size)
                );
        }
    }

    std::size_t alignment() override { return 16; }

    std::size_t align_stack(std::size_t size) {
        const std::size_t a = alignment();
        return (size + a - 1) & ~(a - 1);
    }

    void extern_fn(symbols& sym, std::string& name) override {
        writeln(".extern " + name);
    }

    void prologue(symbols& sym, std::string& name, std::size_t id) override {
        stack_args = pending_cleanup = 0;

        emptylines(1);
        writeln(".globl " + name);
        writeln(".type " + name + ", @function");
        writeln(name + ":");
        writeln(".LFP" + std::to_string(id) + ":");

        set_tabs(1);

        if( name == "_start" ) {
            writeln("xorl %ebp, %ebp");
            writeln("andq $-16, %rsp");
            writeln("movq %rsp, %rbp");
            return;
        }

        writeln("pushq %rbp");
        writeln("movq %rsp, %rbp");
    }

    void epilogue(symbols& sym, std::string& name, std::size_t id) override {
        set_tabs(0);
        writeln(".LFE" + std::to_string(id) + ":");
        set_tabs(1);

        if( name == "_start" ) {
            writeln("ud2");
        } else {
            writeln("leave");
            writeln("ret");
        }

        set_tabs(0);
        writeln(".size " + name + ", . - " + name);
    }

    void argument(symbols& sym, bool lea, std::string& src, std::size_t index, bool sret) override {
        if( sret ) index++;

        if( index < arg_register_count ) {
            writeln(
                std::string(lea ? "leaq " : "movq ")
                + src + ", " + registers[index]
            );
            return;
        }

        if( stack_args == 0 ) {
            const std::size_t total = index - arg_register_count + 1;
            if( total % 2 != 0 ) {
                writeln("subq $8, %rsp");
                pending_cleanup += 8;
            }
        }

        if( lea ) {
            writeln("leaq " + src + ", %rax");
            writeln("pushq %rax");
        } else {
            writeln("pushq " + src);
        }

        stack_args++;
        pending_cleanup += 8;
    }

    void call(symbols& sym, std::string& label) override {
        writeln("call " + label);

        if( pending_cleanup != 0 ) {
            writeln("addq $" + std::to_string(pending_cleanup) + ", %rsp");
        }
        stack_args = pending_cleanup = 0;
    }

    void collect_ret(symbols& sym, std::size_t pos, std::size_t size, bool sret) override {
        if( sret ) return;

        writeln(
            std::string(move_instruction(size)) + " "
            + accumulator(size) + ", " + generate_position(pos)
        );
    }

    void load(symbols& sym, std::size_t pos) override {
        writeln("movq " + generate_position(pos) + ", %rax");
    }

    void store(symbols& sym, std::size_t size, bool lea, std::string& src, std::size_t pos) override {
        const std::string dst = generate_position(pos);

        if( lea ) {
            writeln("leaq " + src + ", %rax");
            writeln("movq %rax, " + dst);
            return;
        }

        const char* mov = move_instruction(size);
        const char* reg = accumulator(size);

        writeln(std::string(mov) + " " + src + ", " + reg);
        writeln(std::string(mov) + " " + reg + ", " + dst);
    }

    void sret(symbols& sym, std::size_t pos) override {
        writeln("leaq " + generate_position(pos) + ", %rdi");
    }

    void save_sret(symbols& sym, std::size_t pos) override {
        sret_slot = pos;
        writeln("movq %rdi, " + generate_position(pos));
    }

    void ret_value(symbols& sym, std::size_t pos, std::size_t size, bool tuple, std::size_t id) override {
        if( tuple ) {
            if( sret_slot == 0 )
                throw std::runtime_error("ret of a tuple in a function without an sret slot");

            writeln("movq " + generate_position(sret_slot) + ", %rdi");
            writeln("leaq " + generate_position(pos) + ", %rsi");
            writeln("movq $" + std::to_string(size) + ", %rcx");
            writeln("rep movsb");

            writeln("movq " + generate_position(sret_slot) + ", %rax");
        } else {
            writeln(
                std::string(move_instruction(size)) + " "
                + generate_position(pos) + ", " + accumulator(size)
            );
        }

        writeln("jmp .LFE" + std::to_string(id));
    }

    void param(symbols& sym, std::size_t index, std::size_t pos,
                   std::size_t size, bool sret) override {
            if( sret ) index++;

            const std::string dst = generate_position(pos);

            if( index < arg_register_count ) {
                writeln(
                    std::string(move_instruction(size)) + " "
                    + param_register(index, size) + ", " + dst
                );
                return;
            }

            const std::size_t offset = 16 + 8 * (index - arg_register_count);

            writeln("movq " + std::to_string(offset) + "(%rbp), %rax");
            writeln(
                std::string(move_instruction(size)) + " "
                + accumulator(size) + ", " + dst
            );
        }

    void alloca(std::size_t size) override {
        size = align_stack(size);
        if( size == 0 ) return;

        writeln("subq $" + std::to_string(size) + ", %rsp");
    }

    void ret_int(symbols& sym, std::string& data, std::size_t id) override {
        writeln("movq $" + data + ", %rax");
        writeln("jmp .LFE" + std::to_string(id));
    }
};
