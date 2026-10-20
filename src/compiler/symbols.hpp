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

struct unfold {
    types::type ret;
    std::vector<types::type> args;

    unfold(types::type ret, std::vector<types::type> args)
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

using symbol = std::variant<std::monostate, subroutine, allocation, borrowed, unfold>;

struct symbols {
    std::vector<std::map<std::string, symbol>> sym_tabs;

    symbols() { sym_tabs.emplace_back(); }

    void push_scope() { sym_tabs.emplace_back(); }

    void pop_scope() {
        if( sym_tabs.size() > 1 ) sym_tabs.pop_back();
    }

    void push(std::string id, symbol sym) {
        sym_tabs.back().insert_or_assign(std::move(id), std::move(sym));
    }

    std::optional<symbol> lookup(const std::string& id) const {
        for( auto tab = sym_tabs.rbegin(); tab != sym_tabs.rend(); ++tab ) {
            auto it = tab->find(id);
            if( it != tab->end() ) return it->second;
        }
        return std::nullopt;
    }

    bool erase(const std::string& id) {
        for( auto tab = sym_tabs.rbegin(); tab != sym_tabs.rend(); ++tab ) {
            if( tab->erase(id) > 0 ) return true;
        }
        return false;
    }

    bool erase_local(const std::string& id) {
        return sym_tabs.back().erase(id) > 0;
    }
};
