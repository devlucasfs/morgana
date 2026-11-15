#pragma once

#include <cstddef>
#include <string>

#include "src/codegen/interface.hpp"

#define repeat3(x) (x), (x), (x)
#define repeat2(x) (x), (x)

class x86_64_gnu : public codegen_interface_methods {
private:
    const std::string entrypoint = "_start";
    std::size_t stack_sz = 0;
    std::string current;
    int func = 0;

protected:
    std::size_t normalized_stacksz() {
        return (stack_sz + 15) & ~static_cast<std::size_t>(15);
    }

public:
    using codegen_interface_methods::codegen_interface_methods;

    void prologue(prologue_t prologue) override {
        stack_sz = 0;
        current  = prologue.identifier;

        const char *id = c(current);

        into = false;
        emit(".text\n"
             ".globl %s\n"
             ".type %s, @function\n"
             "%s:\n"
             ".LFP%d:\n",
             repeat3(id),
             func);

        into = true;

        if( current == entrypoint ) {
            emit("xorl %%ebp, %%ebp\n"
                 "andq $-16, %%rsp\n"
                 "movq %%rsp, %%rbp\n");
        } else {
            emit("pushq %%rbp\n"
                 "movq %%rsp, %%rbp\n");
        }

        emit("subq $.LFRAME%d, %%rsp\n", func);
    }

    void epilogue(epilogue_t epilogue) override {
        const char *id = c(current);

        into = false;
        emit(".set .LFRAME%d, %zu\n", func,  normalized_stacksz());
        emit(".LFE%d:\n", func);

        into = true;

        if( current == entrypoint ) {
            emit("movq %%rax, %%rdi\n"
                 "movl $60, %%eax\n"
                 "syscall\n");
        } else {
            emit("leave\n"
                 "ret\n");
        }

        into = false;
        emit(".size %s, .-%s\n\n", repeat2(id));

        func++;
    }
};
