#pragma once

#include <string>
constexpr unsigned const_hash(const char *input) {
    return (*input)
        ? static_cast<unsigned>(*input) + 33 * const_hash(input + 1)
        : 5381;
}

unsigned hash(std::string str) {
    const char *input = str.c_str();
    return (*input)
        ? static_cast<unsigned>(*input) + 33 * const_hash(input + 1)
        : 5381;
}
