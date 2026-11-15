#pragma once
#include <stdexcept>
using cstring = const char *;
#define mangled(id) __morgana_##id

#define try_pattern(block) {                                  \
    try block                                                 \
    catch (const std::runtime_error& e) {                     \
        std::cerr << "runtime error in Morgana " << e.what(); \
        std::exit(14);                                        \
    }                                                         \
}
