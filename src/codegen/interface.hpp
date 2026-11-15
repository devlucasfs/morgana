#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>

#include "../recognize/parser.hpp"

/* Helper: std::string -> const char*, usable inside printf-style calls. */
inline const char *c(const std::string &s) {
    return s.c_str();
}

class codegen_interface_methods {
protected:
    FILE *output;

public:
    /* When true, every line written through emit() gets a '\t' prefix. */
    bool into = false;

    explicit codegen_interface_methods(FILE *output)
        : output(output) {}

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

    virtual void prologue(prologue_t prologue) = 0;
    virtual void epilogue(epilogue_t epilogue) = 0;
};
