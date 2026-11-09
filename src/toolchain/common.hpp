#pragma once

#include <string>

void replace(std::string& cpp, const std::string& key, const std::string& value) {
    std::string placeholder = "$" + key;
    std::string replacement = "\"" + value + "\"";

    size_t pos = 0;
    while( (pos = cpp.find(placeholder, pos)) != std::string::npos ) {
        cpp.replace(pos, placeholder.length(), replacement);
        pos += replacement.length();
    }
};
