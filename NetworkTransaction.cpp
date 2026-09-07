#include "NetworkTransaction.hpp"

#include <iostream>
#include <stdexcept>
#include <utility>

void NetworkTransaction::apply(std::string name, const std::function<void()>& action,
                               std::function<void()> undo) {
    steps.push_back({std::move(name), std::move(undo), false});
    action();
    steps.back().applied = true;
}

void NetworkTransaction::rollback() {
    std::string errors;
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
        if (!it->applied) continue;
        try {
            it->undo();
            it->applied = false;
        } catch (const std::exception& error) {
            errors += it->name + ": " + error.what() + "\n";
        }
    }
    if (!errors.empty()) {
        throw std::runtime_error("Network cleanup failed:\n" + errors);
    }
    steps.clear();
}

NetworkTransaction::~NetworkTransaction() {
    try {
        rollback();
    } catch (const std::exception& error) {
        std::cerr << error.what();
    }
}
