#include "chronos/vector_index.hpp"
#include "chronos/llm_config.hpp"
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <cmath>
#include <iostream>
#include <chrono>

#include <hnswlib/hnswlib.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace chronos {

namespace {
double getEnvDouble(const char* name, double defaultValue, double minVal, double maxVal = -1.0) {
    const char* envVal = std::getenv(name);
    if (!envVal || *envVal == '\0') {
        return defaultValue;
    }
    try {
        double val = std::stod(envVal);
        if (val < minVal) return minVal;
        if (maxVal >= minVal && val > maxVal) return maxVal;
        return val;
    } catch (...) {
        return defaultValue;
    }
}
} // namespace

MemoryTier getMemoryTier(int64_t commitTimestamp, int64_t currentTimestamp) {
    if (currentTimestamp == 0) {
        currentTimestamp = static_cast<int64_t>(std::time(nullptr));
    }
    if (commitTimestamp <= 0 || commitTimestamp >= currentTimestamp) {
        return MemoryTier::Hot;
    }
    int64_t age = currentTimestamp - commitTimestamp;
    constexpr int64_t kOneYearSecs = 365LL * 86400LL;      // 31,536,000 s
    constexpr int64_t kThirtyDaysSecs = 30LL * 86400LL;   // 2,592,000 s

    if (age > kOneYearSecs) {
        return MemoryTier::Cold;
    } else if (age > kThirtyDaysSecs) {
        return MemoryTier::Warm;
    } else {
        return MemoryTier::Hot;
    }
}

SQ8Vector SQ8Vector::quantize(const std::vector<float>& vec) {
    SQ8Vector sq;
    if (vec.empty()) return sq;

    sq.data.resize(vec.size());
    float minV = vec[0];
    float maxV = vec[0];
    for (float v : vec) {
        if (v < minV) minV = v;
        if (v > maxV) maxV = v;
    }

    sq.minVal = minV;
    float range = maxV - minV;
    if (range < 1e-8f) {
        sq.scale = 1.0f;
    } else {
        sq.scale = range / 255.0f;
    }

    for (size_t i = 0; i < vec.size(); ++i) {
        float normalized = (vec[i] - sq.minVal) / sq.scale;
        int val = static_cast<int>(std::round(normalized)) - 128;
        if (val < -128) val = -128;
        if (val > 127) val = 127;
        sq.data[i] = static_cast<int8_t>(val);
    }

    return sq;
}

std::vector<float> SQ8Vector::dequantize() const {
    std::vector<float> vec(data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        int unsignedVal = static_cast<int>(data[i]) + 128;
        vec[i] = minVal + static_cast<float>(unsignedVal) * scale;
    }
    return vec;
}

size_t SQ8Vector::sizeBytes() const {
    return data.size() * sizeof(int8_t) + sizeof(minVal) + sizeof(scale);
}

struct VectorIndex::Impl {
    std::unordered_map<std::string, hnswlib::labeltype> id_to_label;
    std::unordered_map<hnswlib::labeltype, std::string> label_to_id;
    std::unordered_map<hnswlib::labeltype, int64_t> timestamps;
    std::unordered_map<hnswlib::labeltype, SQ8Vector> quantized_vectors;
    hnswlib::labeltype next_label = 0;
    
    hnswlib::L2Space* space;
    hnswlib::HierarchicalNSW<float>* alg_hnsw;
    
    Impl() {
        space = new hnswlib::L2Space(VectorIndex::kDim);
        alg_hnsw = new hnswlib::HierarchicalNSW<float>(space, 100000, 16, 200);
    }
    ~Impl() {
        delete alg_hnsw;
        delete space;
    }
};

VectorIndex::VectorIndex(const std::string& repoRoot) {
    fs::path chronosDir = fs::path(repoRoot) / ".chronos";
    fs::create_directories(chronosDir);
    path_ = (chronosDir / "vectors.bin").string();
    impl_ = new Impl();

    if (fs::exists(path_)) {
        impl_->alg_hnsw->loadIndex(path_, impl_->space);
        
        std::string meta_path = (chronosDir / "vectors_meta.bin").string();
        if (fs::exists(meta_path)) {
            std::ifstream in(meta_path, std::ios::binary);
            uint32_t magic = 0;
            in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
            if (magic == 0x53513831) {
                in.read(reinterpret_cast<char*>(&impl_->next_label), sizeof(impl_->next_label));
                size_t count = 0;
                in.read(reinterpret_cast<char*>(&count), sizeof(count));
                for (size_t i = 0; i < count; ++i) {
                    hnswlib::labeltype label;
                    in.read(reinterpret_cast<char*>(&label), sizeof(label));
                    
                    uint32_t idLen = 0;
                    in.read(reinterpret_cast<char*>(&idLen), sizeof(idLen));
                    std::string id(idLen, '\0');
                    in.read(id.data(), idLen);
                    
                    int64_t ts = 0;
                    in.read(reinterpret_cast<char*>(&ts), sizeof(ts));
                    
                    SQ8Vector sq;
                    in.read(reinterpret_cast<char*>(&sq.minVal), sizeof(sq.minVal));
                    in.read(reinterpret_cast<char*>(&sq.scale), sizeof(sq.scale));
                    uint32_t dataLen = 0;
                    in.read(reinterpret_cast<char*>(&dataLen), sizeof(dataLen));
                    sq.data.resize(dataLen);
                    if (dataLen > 0) {
                        in.read(reinterpret_cast<char*>(sq.data.data()), dataLen);
                    }

                    impl_->id_to_label[id] = label;
                    impl_->label_to_id[label] = id;
                    impl_->timestamps[label] = ts;
                    impl_->quantized_vectors[label] = std::move(sq);
                }
            } else {
                in.seekg(0, std::ios::beg);
                in.read(reinterpret_cast<char*>(&impl_->next_label), sizeof(impl_->next_label));
                size_t count = 0;
                in.read(reinterpret_cast<char*>(&count), sizeof(count));
                for (size_t i = 0; i < count; ++i) {
                    hnswlib::labeltype label;
                    in.read(reinterpret_cast<char*>(&label), sizeof(label));
                    
                    uint32_t idLen = 0;
                    in.read(reinterpret_cast<char*>(&idLen), sizeof(idLen));
                    std::string id(idLen, '\0');
                    in.read(id.data(), idLen);
                    
                    int64_t ts = 0;
                    in.read(reinterpret_cast<char*>(&ts), sizeof(ts));
                    
                    impl_->id_to_label[id] = label;
                    impl_->label_to_id[label] = id;
                    impl_->timestamps[label] = ts;
                }
            }
        }
    }
}

VectorIndex::~VectorIndex() {
    impl_->alg_hnsw->saveIndex(path_);
    
    std::string meta_path = path_;
    meta_path.replace(meta_path.find("vectors.bin"), 11, "vectors_meta.bin");
    std::ofstream out(meta_path, std::ios::binary | std::ios::trunc);
    
    uint32_t magic = 0x53513831;
    out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    out.write(reinterpret_cast<const char*>(&impl_->next_label), sizeof(impl_->next_label));
    size_t count = impl_->id_to_label.size();
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));
    
    for (const auto& [id, label] : impl_->id_to_label) {
        out.write(reinterpret_cast<const char*>(&label), sizeof(label));
        uint32_t idLen = static_cast<uint32_t>(id.size());
        out.write(reinterpret_cast<const char*>(&idLen), sizeof(idLen));
        out.write(id.data(), idLen);
        
        int64_t ts = impl_->timestamps[label];
        out.write(reinterpret_cast<const char*>(&ts), sizeof(ts));

        const auto& sq = impl_->quantized_vectors[label];
        out.write(reinterpret_cast<const char*>(&sq.minVal), sizeof(sq.minVal));
        out.write(reinterpret_cast<const char*>(&sq.scale), sizeof(sq.scale));
        uint32_t dataLen = static_cast<uint32_t>(sq.data.size());
        out.write(reinterpret_cast<const char*>(&dataLen), sizeof(dataLen));
        if (dataLen > 0) {
            out.write(reinterpret_cast<const char*>(sq.data.data()), dataLen);
        }
    }
    
    delete impl_;
}

void VectorIndex::upsert(const EmbeddingRecord& rec) {
    if (rec.tier == MemoryTier::Cold) {
        return; // Skip vector insertion for Cold Tier commits (> 1 year old)
    }

    if (impl_->alg_hnsw->cur_element_count >= impl_->alg_hnsw->max_elements_) {
        // Prevent OOM on constrained test environments
        if (impl_->alg_hnsw->max_elements_ < 800000) {
            try {
                impl_->alg_hnsw->resizeIndex(impl_->alg_hnsw->max_elements_ + 10000); // Grow linearly instead of doubling to save peak memory spike
            } catch (...) {
                return; // Memory allocation failed, skip adding this vector
            }
        } else {
            return; // Hard cap reached
        }
    }
    hnswlib::labeltype label;
    if (impl_->id_to_label.count(rec.nodeId)) {
        label = impl_->id_to_label[rec.nodeId];
    } else {
        label = impl_->next_label++;
        impl_->id_to_label[rec.nodeId] = label;
        impl_->label_to_id[label] = rec.nodeId;
    }
    
    impl_->timestamps[label] = rec.timestamp;
    SQ8Vector sq = SQ8Vector::quantize(rec.vector);
    impl_->quantized_vectors[label] = sq;

    std::vector<float> deq = sq.dequantize();
    impl_->alg_hnsw->addPoint(deq.data(), label);
}

void VectorIndex::remove(const std::string& nodeId) {
    if (impl_->id_to_label.count(nodeId)) {
        hnswlib::labeltype label = impl_->id_to_label[nodeId];
        impl_->alg_hnsw->markDelete(label);
        impl_->id_to_label.erase(nodeId);
        impl_->label_to_id.erase(label);
        impl_->timestamps.erase(label);
        impl_->quantized_vectors.erase(label);
    }
}

size_t VectorIndex::size() const {
    return impl_->id_to_label.size();
}

bool VectorIndex::contains(const std::string& nodeId) const {
    return impl_->id_to_label.count(nodeId) > 0;
}

std::vector<SeedMatch> VectorIndex::search(const std::vector<float>& queryVector, int topK, int64_t queryTimestamp) const {
    if (queryTimestamp == 0) {
        queryTimestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    
    std::vector<SeedMatch> results;
    if (impl_->alg_hnsw->cur_element_count == 0) return results;
    
    // Fetch a bit more to re-rank with temporal decay
    int fetch_k = std::min<int>(topK * 3, impl_->alg_hnsw->cur_element_count);
    auto result_queue = impl_->alg_hnsw->searchKnn(queryVector.data(), fetch_k);
    
    const double alpha = getEnvDouble("CHRONOS_RECENCY_ALPHA", 0.7, 0.0, 1.0);
    const double lambda = getEnvDouble("CHRONOS_RECENCY_LAMBDA", 1e-7, 0.0, -1.0);
    
    while (!result_queue.empty()) {
        auto top = result_queue.top();
        result_queue.pop();
        
        hnswlib::labeltype label = top.second;
        if (impl_->label_to_id.count(label) == 0) continue;
        
        float dist = top.first;
        // Convert L2 squared distance roughly to cosine similarity (if normalized vectors)
        float cos_sim = 1.0f - (dist / 2.0f);
        
        int64_t commit_ts = impl_->timestamps[label];
        double deltaT = (commit_ts > 0 && queryTimestamp > commit_ts)
                            ? static_cast<double>(queryTimestamp - commit_ts)
                            : 0.0;
        
        // Exponential temporal decay formula
        float final_score = static_cast<float>((alpha * cos_sim) + ((1.0 - alpha) * std::exp(-lambda * deltaT)));
        
        results.push_back({impl_->label_to_id[label], final_score});
    }
    
    std::sort(results.begin(), results.end(), [](const SeedMatch& a, const SeedMatch& b) {
        return a.score > b.score;
    });
    
    if (static_cast<int>(results.size()) > topK) results.resize(topK);
    return results;
}

std::vector<float> embedText(const std::string& text) {
    std::vector<float> vec(VectorIndex::kDim, 0.f);
    if (text.empty()) return vec;

    LlmConfig conf = loadLlmConfig();
    if (!conf.apiUrl.empty()) {
        std::string embedUrl = conf.apiUrl;
        size_t pos = embedUrl.find("/chat/completions");
        if (pos != std::string::npos) {
            embedUrl.replace(pos, 17, "/embeddings");
        }

        std::string escapedText;
        for (char c : text) {
            if (c == '"' || c == '\\') escapedText += '\\';
            else if (c == '\n') escapedText += "\\n";
            else escapedText += c;
        }
        
        std::string body = "{\"input\":\"" + escapedText + "\",\"model\":\"" + conf.modelName + "\"}";
        
        std::string tmpFile = "/tmp/chronos_vec_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".json";
        {
            std::ofstream out(tmpFile);
            out << body;
        }

        std::string authHeader = conf.apiKey.empty() ? "" : "-H \"Authorization: Bearer " + conf.apiKey + "\" ";
        std::string cmd = "curl -s " + embedUrl + " "
                          "-H \"Content-Type: application/json\" "
                          + authHeader +
                          "-d @" + tmpFile;
                              
        FILE* pipe = popen(cmd.c_str(), "r");
        if (pipe) {
            char buffer[128];
            std::string result = "";
            while (!feof(pipe)) {
                if (fgets(buffer, 128, pipe) != nullptr) result += buffer;
            }
            pclose(pipe);
            std::remove(tmpFile.c_str());
            
            size_t dataPos = result.find("\"embedding\": [");
            if (dataPos != std::string::npos) {
                size_t start = dataPos + 14;
                size_t end = result.find("]", start);
                if (end != std::string::npos) {
                    std::string vecStr = result.substr(start, end - start);
                    std::vector<float> v;
                    std::istringstream ss(vecStr);
                    std::string token;
                    while (std::getline(ss, token, ',')) {
                        v.push_back(std::stof(token));
                    }
                    if (v.size() > 0) {
                        for (size_t i = 0; i < std::min<size_t>(v.size(), VectorIndex::kDim); ++i) vec[i] = v[i];
                        return vec;
                    }
                }
            }
        }
    }
    
    // Fallback: deterministic hashing if Ollama is not running (fail-safe for CI/Tests)
    std::string token;
    auto flush = [&]() {
        if (token.empty()) return;
        uint64_t h = 1469598103934665603ULL;
        for (unsigned char c : token) { h ^= c; h *= 1099511628211ULL; }
        int bucket = static_cast<int>(h % VectorIndex::kDim);
        float sign = ((h >> 63) & 1ULL) ? -1.f : 1.f;
        vec[bucket] += sign;
        token.clear();
    };
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) token += static_cast<char>(std::tolower(c));
        else flush();
    }
    flush();
    
    double norm = 0;
    for (float v : vec) norm += v * v;
    norm = std::sqrt(norm);
    if (norm > 0) for (float& v : vec) v = static_cast<float>(v / norm);
    return vec;
}

} // namespace chronos
