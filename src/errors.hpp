#pragma once

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <filesystem>

#include "recognize/lexer.hpp"

#define ANSI_BLUE   "\033[1;34m"
#define ANSI_RED    "\033[1;31m"
#define ANSI_YELLOW "\033[1;33m"
#define ANSI_RESET  "\033[0m"

void syntax_error(std::filesystem::path path, Tk token, std::string data) {
    std::ifstream file(path);

    if(! file ) {
        std::cerr << "Failed to open file: " << path << '\n';
        return;
    }

    std::string line;
    std::size_t current = 1;

    std::size_t first = token.line > 1 ? token.line - 1 : 1;
    std::size_t last = token.line + 1;
    std::size_t width = std::to_string(last).size();

    while(std::getline(file, line)) {
        if( current >= first && current <= last ) {
            bool error_line = current == token.line;

            std::cerr
                << (((error_line)) ? ANSI_RED "> " : "  ")
                << ANSI_BLUE << std::setw(width) << current << " | "
                << ANSI_RESET;

            if( error_line ) {
                size_t column = (token.col > 0) ? token.col - 1 : 0;
                column = std::min(column, line.size());

                size_t length = std::min(
                    token.lex.size(),
                    line.size() - column
                );

                std::cerr
                    << line.substr(0, column)
                    << ANSI_YELLOW << "\033[1m" << line.substr(column, length)
                    << ANSI_RESET << line.substr(column + length) << '\n';
                goto __ignore_next_op;
            }

            std::cerr << line << '\n';
            __ignore_next_op:;
        }

        if( current >= last ) break;
        ++current;
    }

    std::cerr << ANSI_RED << "\nSyntax error: "
              << ANSI_RESET << data  << '\n';

    std::exit(14);
}
