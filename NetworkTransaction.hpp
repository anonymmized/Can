#pragma once

#include <functional>
#include <string>
#include <vector>

class NetworkTransaction {
public:
    ~NetworkTransaction();
    NetworkTransaction() = default;
    NetworkTransaction(const NetworkTransaction&) = delete;
    NetworkTransaction& operator=(const NetworkTransaction&) = delete;

    void apply(std::string name, const std::function<void()>& action,
               std::function<void()> undo);
    void rollback();

private:
    struct Step {
        std::string name;
        std::function<void()> undo;
        bool applied = false;
    };
    std::vector<Step> steps;
};
