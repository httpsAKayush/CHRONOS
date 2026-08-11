#pragma once

#include "chronos/core/ICommand.hpp"
#include "chronos/cli/cli_context.hpp"

namespace chronos {

class CmdInit : public ICommand {
public:
    explicit CmdInit(const CliContext& ctx);
    int execute(int argc, char** argv) override;
    std::string name() const override { return "init"; }
    std::string description() const override { return "Initialize a new chronos repository"; }

private:
    const CliContext& ctx_;
};

class CmdSync : public ICommand {
public:
    explicit CmdSync(const CliContext& ctx);
    int execute(int argc, char** argv) override;
    std::string name() const override { return "sync"; }
    std::string description() const override { return "Index the codebase, parse ASTs, and build the graph"; }

private:
    const CliContext& ctx_;
};

class CmdAsk : public ICommand {
public:
    explicit CmdAsk(const CliContext& ctx);
    int execute(int argc, char** argv) override;
    std::string name() const override { return "ask"; }
    std::string description() const override { return "The Macroscopic Pathfinder. Ask a conceptual question"; }

private:
    const CliContext& ctx_;
};

class CmdExplain : public ICommand {
public:
    explicit CmdExplain(const CliContext& ctx);
    int execute(int argc, char** argv) override;
    std::string name() const override { return "explain"; }
    std::string description() const override { return "The Microscopic Interrogator. Explain a specific symbol"; }

private:
    const CliContext& ctx_;
};

class CmdMap : public ICommand {
public:
    explicit CmdMap(const CliContext& ctx);
    int execute(int argc, char** argv) override;
    std::string name() const override { return "map"; }
    std::string description() const override { return "The Architectural X-Ray. Shows skeletal view of function behavior"; }

private:
    const CliContext& ctx_;
};

class CmdDiagnose : public ICommand {
public:
    explicit CmdDiagnose(const CliContext& ctx);
    int execute(int argc, char** argv) override;
    std::string name() const override { return "diagnose"; }
    std::string description() const override { return "The Ghost Bug Diagnosis Engine. Walk backward from a stack trace"; }

private:
    const CliContext& ctx_;
};

class CmdTrace : public ICommand {
public:
    explicit CmdTrace(const CliContext& ctx);
    int execute(int argc, char** argv) override;
    std::string name() const override { return "trace"; }
    std::string description() const override { return "Inspect the exact context graph for a trace ID"; }

private:
    const CliContext& ctx_;
};

class CmdTimeline : public ICommand {
public:
    explicit CmdTimeline(const CliContext& ctx);
    int execute(int argc, char** argv) override;
    std::string name() const override { return "timeline"; }
    std::string description() const override { return "See the history of structural mutations for a file/symbol"; }

private:
    const CliContext& ctx_;
};

class CmdCheckStaging : public ICommand {
public:
    explicit CmdCheckStaging(const CliContext& ctx);
    int execute(int argc, char** argv) override;
    std::string name() const override { return "check-staging"; }
    std::string description() const override { return "Prevent Temporal Collisions before committing"; }

private:
    const CliContext& ctx_;
};

class CmdStatus : public ICommand {
public:
    explicit CmdStatus(const CliContext& ctx);
    int execute(int argc, char** argv) override;
    std::string name() const override { return "status"; }
    std::string description() const override { return "Check daemon health, graph size, and system state"; }

private:
    const CliContext& ctx_;
};

class CmdConfig : public ICommand {
public:
    explicit CmdConfig(const CliContext& ctx);
    int execute(int argc, char** argv) override;
    std::string name() const override { return "config"; }
    std::string description() const override { return "Manage LLM keys, AI providers, and environment variables"; }

private:
    const CliContext& ctx_;
};

class CmdExport : public ICommand {
public:
    explicit CmdExport(const CliContext& ctx);
    int execute(int argc, char** argv) override;
    std::string name() const override { return "export"; }
    std::string description() const override { return "Dump the structural graph data to JSON or Mermaid"; }

private:
    const CliContext& ctx_;
};

class CmdClean : public ICommand {
public:
    explicit CmdClean(const CliContext& ctx);
    int execute(int argc, char** argv) override;
    std::string name() const override { return "clean"; }
    std::string description() const override { return "Destroy the local index and free up disk space"; }

private:
    const CliContext& ctx_;
};

class CmdCommit : public ICommand {
public:
    explicit CmdCommit(const CliContext& ctx);
    int execute(int argc, char** argv) override;
    std::string name() const override { return "commit"; }
    std::string description() const override { return "Generate a Conventional Commit message from staged changes and commit"; }

private:
    const CliContext& ctx_;
};

} // namespace chronos