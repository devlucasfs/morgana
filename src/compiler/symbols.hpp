#pragma once

#include "parser/types.hpp"
#include <map>
#include <optional>
#include <stack>
#include <string>
#include <variant>
#include <vector>

struct subroutine {
    types::type ret;
    std::vector<types::type> args;

    subroutine(types::type ret, std::vector<types::type> args)
        : ret(ret), args(args) {}
};

struct allocation {
    types::type allocated;
    size_t pos;

    allocation(types::type type, size_t pos)
        : allocated(type), pos(pos) {}
};

struct borrowed {
    types::type allocated;
    size_t pos;

    borrowed(types::type type, size_t pos)
        : allocated(type), pos(pos) {}
};

using symbol = std::variant<std::monostate, subroutine, allocation, borrowed>;

struct symbols {
    std::stack<std::map<std::string, symbol>> sym_tabs;
    symbols() {
        sym_tabs.push({});
    }

    void push_scope() { sym_tabs.push({}); }
    void pop_scope() {
        if( sym_tabs.size() > 1 ) {
            sym_tabs.pop();
        }
    }

    void push(std::string id, symbol sym) {
        sym_tabs.top().insert({id, sym});
    }

    std::optional<symbol> lookup(const std::string& id) {
        auto tabs = sym_tabs;
        while(!tabs.empty()) {
            auto& tab = tabs.top();
            auto it = tab.find(id);
            if( it != tab.end() ) return it->second;
            tabs.pop();
        }
        return std::nullopt;
    }
};
