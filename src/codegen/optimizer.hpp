#pragma once

#include <cstddef>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "../recognize/parser.hpp"
#include "./codegen/make.hpp"

struct opt_options {
    std::unordered_set<std::string> roots = { "main" };
    bool prune_declarations = true;
};

namespace opt_detail {
    inline constexpr std::size_t npos = static_cast<std::size_t>(-1);
    struct function_info {
        std::string name;
        std::vector<std::string> refs;
        bool live = false;
        bool exported = false;   // `export`: always kept, even if nothing references it
    };
}

inline Ins optimize(Ins ins, const opt_options& opt = {}) {
    using namespace opt_detail;

    std::vector<function_info> funcs;
    std::unordered_map<std::string, std::vector<std::size_t>> by_name;
    std::vector<std::string> global_refs;
    std::vector<std::size_t> owner(ins.set.size(), npos);

    auto add_ref = [&](std::size_t open, const std::string& name) {
        if(open == npos)
            global_refs.push_back(name);
        else
            funcs[open].refs.push_back(name);
    };

    std::size_t open = npos;

    for( std::size_t i = 0; i < ins.set.size(); i++ ) {
        const auto& it = ins.set[i];

        if( const auto* p = std::get_if<prologue>(&it) ) {
            funcs.push_back({ p->identifier, {}, false, p->exported });
            open = funcs.size() - 1;
            by_name[p->identifier].push_back(open);
            owner[i] = open;
            continue;
        }

        if( open != npos ) owner[i] = open;

        if( std::holds_alternative<epilogue>(it) ) {
            open = npos;
            continue;
        }

        if( const auto* c = std::get_if<call_op>(&it) ) {
            add_ref(open, c->callee);
            continue;
        }

        if( const auto* p = std::get_if<ptr_to_op>(&it) ) {
            if( p->function ) add_ref(open, p->target);
            continue;
        }
    }

    std::vector<std::size_t> work;

    auto mark = [&](const std::string& name) {
        auto it = by_name.find(name);
        if( it == by_name.end() ) return;

        for( std::size_t id : it->second ) {
            if(! funcs[id].live ) {
                funcs[id].live = true;
                work.push_back(id);
            }
        }
    };

    for( const auto& root : opt.roots )   mark(root);
    for( const auto& name : global_refs ) mark(name);

    for( std::size_t id = 0; id < funcs.size(); id++ ) {
        if( funcs[id].exported && !funcs[id].live ) {
            funcs[id].live = true;
            work.push_back(id);
        }
    }

    while(!work.empty()) {
        std::size_t id = work.back();
        work.pop_back();
        for( const auto& name : funcs[id].refs ) mark(name);
    }

    Ins result;
    result.set.reserve(ins.set.size());

    for( std::size_t i = 0; i < ins.set.size(); i++ ) {
        std::size_t o = owner[i];
        if( o == npos || funcs[o].live )
            result.set.push_back(std::move(ins.set[i]));
    }

    return result;
}
