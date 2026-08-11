#include "chronos/cli/commands.hpp"
#include "chronos/cli/cli_util.hpp"
#include "chronos/use_cases/oracle.hpp"
#include <iostream>

namespace chronos {

CmdTrace::CmdTrace(const CliContext& ctx) : ctx_(ctx) {}

int CmdTrace::execute(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: chronos trace <traceId>\n";
        return 2;
    }
    Codex& codex = *ctx_.storage;
    Oracle oracle(codex, ctx_.repoRoot);
    auto trace = codex.getTrace(argv[2]);
    if (!trace) {
        std::cerr << "chronos trace: no trace found for id " << argv[2] << "\n";
        return 1;
    }
    std::cout << oracle.renderTrace(*trace);
    return 0;
}

} // namespace chronos