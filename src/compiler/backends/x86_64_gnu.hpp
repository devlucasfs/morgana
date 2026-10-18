
#pragma once

#include "../codegen/backends.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>

class x86_64_gnu : public assembly_backend {
protected:
    std::string generate_position(std::size_t pos) {
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
                    "Unsupported operand size"
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
                    "Unsupported operand size"
                );
        }
    }

public:
    explicit x86_64_gnu(const std::string& output) : assembly_backend(output) {}

    inline static const char* registers[] = {
        "%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"
    };

    std::size_t alignment()
    override { return 16; }

    void extern_fn(symbols& sym, std::string& name)
    override { writeln(".extern " + name); }

    void prologue(symbols& sym, std::string& name, std::size_t id)
    override {
        emptylines(1);
        writeln(".globl " + name);
        writeln(".type " + name + ", @function");
        writeln(name + ":");

        set_tabs(1);

        writeln("pushq %rbp");
        writeln("movq %rsp, %rbp");
        writeln("subq $64, %rsp");
    }

    void epilogue(symbols& sym, std::string& name, std::size_t id) override {
        writeln("leave");
        writeln("ret");
        set_tabs(0);
        writeln(".size " + name + ", .-" + name);
    }

    void argument(symbols& sym, bool lea, std::string& src, std::size_t index, bool sret) {
        if( sret ) index++;

        if( index >= 6 ) {
            writeln("pushq " + src);
            return;
        }

        writeln(
            std::string(lea ? "leaq " : "movq ")
            + src + ", " + registers[index]
        );
    }

    void call(symbols& sym, std::string& label) override {
        writeln("call " + label);
    }

    void collect_ret(symbols& sym, std::size_t pos, std::size_t size, bool sret) override {
        if( sret ) return;
        const char* mov = move_instruction(size);
        const char* reg = accumulator(size);
        writeln(
            std::string(mov) + " " + reg + ", " + generate_position(pos)
        );
    }

    void load(symbols& sym, std::size_t pos) override {
        writeln(
            "movq " + generate_position(pos) + ", %rax"
        );
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

        writeln(
            std::string(mov) + " " + src + ", " + reg
        );

        writeln(
            std::string(mov) + " " + reg + ", " + dst
        );
    }

    void sret(symbols& sym, std::size_t pos) override {
        writeln("leaq -" + std::to_string(pos) + "(%rbp), %rdi");
    };
};
