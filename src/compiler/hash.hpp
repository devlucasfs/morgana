#pragma once

#include <cstdint>
#include <string_view>

constexpr uint32_t hash(std::string_view str) {
    uint32_t h = 5381;
    for( char c : str ) h = ( h * 33 ) ^ static_cast<unsigned char>(c);
    return h;
}
