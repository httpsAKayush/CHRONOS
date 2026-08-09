#pragma once
#include <string>

namespace chronos {

class ICommand {
public:
    virtual ~ICommand() = default;
    virtual int execute(int argc, char** argv) = 0;
    virtual std::string name() const = 0;
    virtual std::string description() const = 0;
};

} // namespace chronos
