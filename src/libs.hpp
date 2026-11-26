#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

enum class lib_kind { shared, static_ };

struct lib_spec {
    lib_kind    kind;
    std::string name;
};

inline bool valid_lib_name(const std::string &name) {
    if( name.empty() ) return false;

    for( char c : name ) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '+';
        if (!ok) return false;
    }

    return true;
}

inline std::vector<lib_spec> parse_libs(const std::string &raw) {
    std::vector<lib_spec> out;

    std::size_t start = 0;
    while(start <= raw.size()) {
        std::size_t end = raw.find(',', start);
        if (end == std::string::npos) end = raw.size();

        std::string item = raw.substr(start, end - start);
        start = end + 1;

        if( item.empty() ) continue;

        std::size_t colon = item.find(':');
        if( colon == std::string::npos )
            throw std::runtime_error("Invalid lib `" + item + "`: expected `<kind>:<name>`.");

        std::string kind = item.substr(0, colon);
        std::string name = item.substr(colon + 1);

        lib_spec spec;

        if     ( kind == "shared" ) spec.kind = lib_kind::shared;
        else if( kind == "static" ) spec.kind = lib_kind::static_;
        else throw std::runtime_error("Unknown lib kind `" + kind + "` in `" + item + "`.");

        if(! valid_lib_name(name) )
            throw std::runtime_error("Invalid lib name `" + name + "` in `" + item + "`.");

        spec.name = name;
        out.push_back(spec);
    }

    return out;
}
