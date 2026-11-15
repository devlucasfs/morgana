#include <cstdio>
#include "src/codegen/interface.hpp"

class x86_64_gnu : public codegen_interface_methods {
    using codegen_interface_methods::codegen_interface_methods;

    int func = 0;
    void prologue(prologue_t prologue) override {
        fprintf(output, ".text\n"
                        ".globl %s\n"
                        ".type %s, @function\n"
                        "%s:\n"
                        ".LFP%d:\n",
                c(prologue.identifier),
                c(prologue.identifier),
                c(prologue.identifier),
                func
        );

        if( prologue.identifier == "_start" ) {
            fprintf(output, "xorl %%ebp, %%ebp\n"
                            "andq $-16, %%rsp\n"
                            "movq %%rsp, %%rbp\n"
            );
        }
        else {
            fprintf(output, "pushq %%rbp\n"
                            "movq %%rsp, %%rbp\n"
            );
        }

        fprintf(output, "subq $.LFRAME%d, %%rsp",
                func
        );
    }

    void epilogue(epilogue_t epilogue) override {}
};
