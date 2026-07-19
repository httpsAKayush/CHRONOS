#pragma once
// AF_UNIX socket IPC (Spec §2 hard constraint: 0600 perms, no TCP; Spec §7
// Daemon RPC contract; Spec §18 Open Question on Ctrl+C/EPIPE handling).
//
// Wire format: newline-delimited JSON objects (NDJSON) over a UNIX domain
// stream socket at /tmp/chronos-<repo-hash>.sock (namespaced per repo so
// multiple projects don't collide). We hand-roll a minimal JSON
// encoder/decoder here rather than pulling a JSON library dependency,
// since the payload shape is fixed and small (see ChronosRequest/Response).

#include <string>
#include <vector>
#include <functional>
#include <optional>

namespace chronos {

struct ContextNode {
    std::string nodeId;
    std::string filePath;
    std::string codeSnippet;
    bool uncertain = false; // Spec §7 Safety Contract trigger
};

// Matches Spec §7 Daemon RPC: POST /v1/chat/completions-shaped payload,
// simplified to what Chronos actually needs to send.
struct ChronosRequest {
    std::string traceId;              // Spec §9 Observability "Traceability ID"
    std::string userQuery;
    std::vector<ContextNode> context;
    bool requireCitations = true;     // FR-8
};

struct ChronosResponseChunk {
    std::string traceId;
    std::string textDelta;
    bool done = false;
    bool unverifiedCitation = false;  // set by daemon if a citation check fails
};

std::string encodeRequest(const ChronosRequest& req);
ChronosRequest decodeRequest(const std::string& json);

std::string encodeChunk(const ChronosResponseChunk& chunk);
ChronosResponseChunk decodeChunk(const std::string& json);

// Returns the per-repo socket path used by both client and daemon, derived
// from a hash of the absolute repo root so concurrent projects never share
// a socket.
std::string socketPathForRepo(const std::string& repoRoot);

class IpcClient {
public:
    // Connects to the daemon socket. Returns false (not throw) on failure
    // so callers can implement the "TUI boots daemon on-demand" unhappy
    // path (Spec §0) without exception-driven control flow.
    bool connect(const std::string& socketPath);

    // Sends the request, then invokes `onChunk` for each streamed response
    // chunk until done=true or the socket closes/errors. Returns false if
    // the daemon was unreachable or died mid-stream (Spec unhappy path:
    // "Daemon unreachable... TUI throws a fast, visible socket error").
    bool sendAndStream(const ChronosRequest& req,
                        const std::function<void(const ChronosResponseChunk&)>& onChunk);

    // Explicit interrupt (Spec §18 Open Question): closes the write half so
    // the daemon's next write triggers EPIPE and it can abort generation to
    // free VRAM immediately rather than finishing an unwatched stream.
    void interrupt();

    ~IpcClient();

private:
    int fd_ = -1;
};

class IpcServer {
public:
    // Binds a 0600 UNIX socket at `socketPath`, unlinking any stale file
    // first ("Connect-and-Unlink" strategy, project.md III). `handler` is
    // invoked per connection on a dedicated thread; it should stream chunks
    // via the passed callback and return when done.
    using Handler = std::function<void(const ChronosRequest&,
                                        const std::function<void(const ChronosResponseChunk&)>&)>;

    bool listen(const std::string& socketPath, Handler handler);

    // Blocks, accepting connections, until stop() is called from another
    // thread (e.g. the idle-timeout watchdog, FR-5).
    void run();
    void stop();

private:
    int listenFd_ = -1;
    std::string socketPath_;
    bool running_ = false;
    Handler handler_;
};

} // namespace chronos
