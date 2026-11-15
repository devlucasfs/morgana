#pragma once
#include <cstdio>
#include "../recognize/parser.hpp"

#define c(str) str.c_str()

class codegen_interface_methods {
protected:
    FILE *output;
public:
    explicit codegen_interface_methods(FILE *output)
        : output(output) {}

    virtual void prologue(prologue_t prologue) = 0;
    virtual void epilogue(epilogue_t epilogue) = 0;
};
