#pragma once

#include <string>
#include <unordered_map>

namespace chronos {

class IConfig {
public:
    virtual ~IConfig() = default;

    virtual std::string get(const std::string& key, const std::string& defaultValue = "") const = 0;
    virtual bool getBool(const std::string& key, bool defaultValue = false) const = 0;
    virtual int getInt(const std::string& key, int defaultValue = 0) const = 0;
    virtual bool has(const std::string& key) const = 0;
    virtual std::unordered_map<std::string, std::string> getAll() const = 0;
};

} // namespace chronos
