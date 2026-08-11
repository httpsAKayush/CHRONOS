#pragma once

#include "chronos/infrastructure/codex.hpp"
#include "chronos/infrastructure/vector_index.hpp"
#include "chronos/infrastructure/config.hpp"
#include "chronos/ipc.hpp"
#include "chronos/use_cases/context_builder.hpp"
#include "chronos/use_cases/oracle.hpp"
#include "chronos/use_cases/diagnose.hpp"
#include <string>
#include <vector>
#include <functional>

namespace chronos {

std::string readSnippetForNode(const Node& n, const std::string& repoRoot);
std::string nodeLabel(const Node& n, const std::string& nodeId, Codex* codex = nullptr);
int countLinesTo(const std::string& repoRoot, const std::string& relPath, int64_t byteOffset);
std::string readFunctionSignature(const Node& n, const std::string& repoRoot, const std::string& symName = "");
std::string nodeAnnotation(const Node& n, const std::string& repoRoot, const std::string& symName = "");
std::string resolveLabel(Codex& codex, const std::string& rawId);

enum class MapMode { BOTH, UPSTREAM, DOWNSTREAM };

struct BfsRow {
    std::string nodeId;
    int depth;
    bool isLast;
    int startLine;
    std::string callSiteText;
};

std::vector<BfsRow> bfsCollect(Codex& codex, const std::string& startId, bool outgoing, int maxDepth);
std::string execCmdOutput(const std::string& cmd);
bool checkStagingCollision(const std::string& stagedLine, const std::string& syntheticMsg);
void wakeDaemon(const std::string& repoRoot);

// True if a quick HTTP GET to the endpoint succeeds (used to distinguish
// "provider unreachable" from "provider returned no usable text").
bool endpointReachable(const std::string& url);

// Human-readable diagnostic describing which LLM provider was active and why
// it could not produce a response. Derived from the persisted config, so the
// user always sees the provider they configured.
std::string llmUnavailableMessage(const Config& cfg);

} // namespace chronos