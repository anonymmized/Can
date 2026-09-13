#include <iostream>
#include "ArgumentHandler.hpp"

int main(int argc, char** argv) {
    try {
        return ArgumentHandler(argc, argv).run();
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
