#include "chronos/ipc.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <thread>
#include <functional>

namespace chronos {

// ---------- Minimal JSON helpers (escape/unescape + flat field scan) -----
// The payload shapes here are fixed and shallow, so a tiny hand-rolled
// codec avoids pulling a JSON dependency into a latency-sensitive IPC path.
namespace {

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            default: out += c;
        }
    }
    return out;
}

// Extracts the raw (still-escaped) value of a top-level "key":"..." string
// field. Returns empty if not found. Sufficient for our flat schemas.
std::string extractStringField(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    size_t end = pos;
    while (end < json.size()) {
        if (json[end] == '\\') { end += 2; continue; }
        if (json[end] == '"') break;
        ++end;
    }
    std::string raw = json.substr(pos, end - pos);
    // unescape
    std::string out;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            if (raw[i + 1] == 'n') { out += '\n'; ++i; continue; }
            if (raw[i + 1] == '"' || raw[i + 1] == '\\') { out += raw[i + 1]; ++i; continue; }
        }
        out += raw[i];
    }
    return out;
}

bool extractBoolField(const std::string& json, const std::string& key, bool defaultVal) {
    std::string needle = "\"" + key + "\":";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return defaultVal;
    pos += needle.size();
    return json.compare(pos, 4, "true") == 0;
}

} // namespace

std::string encodeRequest(const ChronosRequest& req) {
    std::ostringstream out;
    out << "{\"traceId\":\"" << jsonEscape(req.traceId) << "\","
        << "\"userQuery\":\"" << jsonEscape(req.userQuery) << "\","
        << "\"requireCitations\":" << (req.requireCitations ? "true" : "false") << ","
        << "\"context\":[";
    for (size_t i = 0; i < req.context.size(); ++i) {
        const auto& c = req.context[i];
        if (i) out << ",";
        out << "{\"nodeId\":\"" << jsonEscape(c.nodeId) << "\","
            << "\"filePath\":\"" << jsonEscape(c.filePath) << "\","
            << "\"codeSnippet\":\"" << jsonEscape(c.codeSnippet) << "\","
            << "\"uncertain\":" << (c.uncertain ? "true" : "false") << "}";
    }
    out << "]}";
    return out.str();
}

ChronosRequest decodeRequest(const std::string& json) {
    ChronosRequest req;
    req.traceId = extractStringField(json, "traceId");
    req.userQuery = extractStringField(json, "userQuery");
    req.requireCitations = extractBoolField(json, "requireCitations", true);
    
    size_t pos = json.find("\"context\":[");
    if (pos != std::string::npos) {
        pos += 11;
        while (pos < json.size() && json[pos] != ']') {
            if (json[pos] == '{') {
                ContextNode cn;
                size_t p = pos;
                
                auto parseStr = [&](const std::string& key) -> std::string {
                    std::string needle = "\"" + key + "\":\"";
                    p = json.find(needle, p);
                    if (p == std::string::npos) return "";
                    p += needle.size();
                    size_t end = p;
                    while (end < json.size()) {
                        if (json[end] == '\\') { end += 2; continue; }
                        if (json[end] == '"') break;
                        ++end;
                    }
                    std::string raw = json.substr(p, end - p);
                    p = end + 1;
                    
                    std::string out;
                    for (size_t i = 0; i < raw.size(); ++i) {
                        if (raw[i] == '\\' && i + 1 < raw.size()) {
                            if (raw[i + 1] == 'n') { out += '\n'; ++i; continue; }
                            if (raw[i + 1] == '"' || raw[i + 1] == '\\') { out += raw[i + 1]; ++i; continue; }
                        }
                        out += raw[i];
                    }
                    return out;
                };

                cn.nodeId = parseStr("nodeId");
                if (cn.nodeId.empty()) break;
                cn.filePath = parseStr("filePath");
                cn.codeSnippet = parseStr("codeSnippet");
                
                std::string uncertNeedle = "\"uncertain\":";
                p = json.find(uncertNeedle, p);
                if (p != std::string::npos) {
                    p += uncertNeedle.size();
                    cn.uncertain = (json.compare(p, 4, "true") == 0);
                }
                
                req.context.push_back(std::move(cn));
                
                p = json.find('}', p);
                if (p == std::string::npos) break;
                pos = p + 1;
            } else {
                ++pos;
            }
        }
    }
    return req;
}

std::string encodeChunk(const ChronosResponseChunk& c) {
    std::ostringstream out;
    out << "{\"traceId\":\"" << jsonEscape(c.traceId) << "\","
        << "\"textDelta\":\"" << jsonEscape(c.textDelta) << "\","
        << "\"done\":" << (c.done ? "true" : "false") << ","
        << "\"unverifiedCitation\":" << (c.unverifiedCitation ? "true" : "false") << "}";
    return out.str();
}

ChronosResponseChunk decodeChunk(const std::string& json) {
    ChronosResponseChunk c;
    c.traceId = extractStringField(json, "traceId");
    c.textDelta = extractStringField(json, "textDelta");
    c.done = extractBoolField(json, "done", false);
    c.unverifiedCitation = extractBoolField(json, "unverifiedCitation", false);
    return c;
}

std::string socketPathForRepo(const std::string& repoRoot) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char ch : repoRoot) { h ^= ch; h *= 1099511628211ULL; }
    std::ostringstream out;
    out << "/tmp/chronos-" << std::hex << h << ".sock";
    return out.str();
}

// ---------------------------- Client --------------------------------------

bool IpcClient::connect(const std::string& socketPath) {
    fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) return false;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd_);
        fd_ = -1;
        return false; // Spec unhappy path: daemon unreachable
    }
    return true;
}

bool IpcClient::sendAndStream(const ChronosRequest& req,
                               const std::function<void(const ChronosResponseChunk&)>& onChunk) {
    if (fd_ < 0) return false;
    std::string payload = encodeRequest(req) + "\n";
    if (::write(fd_, payload.data(), payload.size()) < 0) return false;

    std::string buffer;
    char tmp[4096];
    while (true) {
        ssize_t n = ::read(fd_, tmp, sizeof(tmp));
        if (n <= 0) return false; // socket closed/EPIPE => "fast, visible socket error"
        buffer.append(tmp, n);
        size_t nl;
        while ((nl = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, nl);
            buffer.erase(0, nl + 1);
            ChronosResponseChunk chunk = decodeChunk(line);
            onChunk(chunk);
            if (chunk.done) return true;
        }
    }
}

void IpcClient::interrupt() {
    // Spec §18 Open Question resolution: half-close the write side. The
    // daemon's next write() to this fd receives EPIPE/SIGPIPE(ignored) and
    // its generation loop checks for that to abort early (see main_daemon).
    if (fd_ >= 0) ::shutdown(fd_, SHUT_WR);
}

IpcClient::~IpcClient() {
    if (fd_ >= 0) ::close(fd_);
}

// ---------------------------- Server ---------------------------------------

bool IpcServer::listen(const std::string& socketPath, Handler handler) {
    socketPath_ = socketPath;
    ::unlink(socketPath.c_str()); // Connect-and-Unlink strategy (project.md III)

    listenFd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd_ < 0) return false;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
    // Hard constraint (Spec §2): 0600 permissions, no network exposure.
    ::chmod(socketPath.c_str(), S_IRUSR | S_IWUSR);
    if (::listen(listenFd_, 16) != 0) return false;

    running_ = true;
    handler_ = std::move(handler);
    return true;
}

void IpcServer::run() {
    while (running_) {
        int clientFd = accept(listenFd_, nullptr, nullptr);
        if (clientFd < 0) {
            if (!running_) break;
            continue;
        }
        std::thread([this, clientFd]() {
            std::string buffer;
            char tmp[4096];
            while (true) {
                ssize_t n = ::read(clientFd, tmp, sizeof(tmp));
                if (n <= 0) break; // client hung up / interrupted (EPIPE case)
                buffer.append(tmp, n);
                size_t nl;
                while ((nl = buffer.find('\n')) != std::string::npos) {
                    std::string line = buffer.substr(0, nl);
                    buffer.erase(0, nl + 1);
                    ChronosRequest req = decodeRequest(line);
                    handler_(req, [clientFd](const ChronosResponseChunk& chunk) {
                        std::string out = encodeChunk(chunk) + "\n";
                        // Ignore SIGPIPE at process level (main_daemon does
                        // this); a failed write here just means the client
                        // interrupted us, which is exactly FR-5's cue to
                        // stop generating and free resources.
                        ::write(clientFd, out.data(), out.size());
                    });
                }
            }
            ::close(clientFd);
        }).detach();
    }
}

void IpcServer::stop() {
    running_ = false;
    if (listenFd_ >= 0) ::close(listenFd_);
    ::unlink(socketPath_.c_str());
}

} // namespace chronos
