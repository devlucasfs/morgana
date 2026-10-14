#pragma once
#include <cstddef>
#include <initializer_list>
#include <iostream>
#include <string>
#include <tuple>
#include <variant>
extern "C" {
#include "../../../libs/runa.h"
}

using cap_data = std::initializer_list<std::pair<std::string, std::variant<size_t, std::string>>>;

static size_t __capsize;
static void pcap(Runa *runa, cap_data ___capdata) {
    __capsize = ___capdata.size();
    for( const auto& [id, data] : ___capdata ) {
        if( std::holds_alternative<size_t>(data) ) {
            runa_push_field(runa, (char*) id.c_str(), RunaValueFFI {
                .tag = runa_integer, .data = { .integer = std::get<size_t>(data) }
            });
        }

        if( std::holds_alternative<std::string>(data) ) {
            runa_push_field(runa, (char*) id.c_str(), RunaValueFFI {
                .tag = runa_string, .data = { .string = std::get<std::string>(data).c_str() }
            });
        }
    }
}

static void generic_cap(Runa *runa) {
    runa_push_table(runa, (char*) "data", __capsize);
}
