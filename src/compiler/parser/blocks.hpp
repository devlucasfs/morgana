#include <string>
#include <vector>

#include "../lexer.hpp"

static std::vector<std::string> get_block(
    Lexer& lex,
    std::string end
) {
    std::vector<std::string> t;
    while( auto data = lex.next() ) {
        if( *data == end ) break;
        if( *data == "," ) continue;
        t.push_back(*data);
    }
    return t;
}
