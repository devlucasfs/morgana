#pragma once

#include <iostream>
#include <cstdlib>
#include <string>

struct CompilerOutputs {
    template<typename T>
    static void fatal(const T& text) {
        std::cout << "\033[1;31m[Morgana FAIL]\033[0m: "
                  << text << '\n';

        std::exit(14);
    }

    template<typename T>
    static void fatal_internal(const T& text) {
        std::cout << "\033[1;31m[Morgana FAIL]\033[0m: "
                  << "\033[1;33minternal error!\033[0m "
                  << text << '\n';

        std::exit(14);
    }
};
