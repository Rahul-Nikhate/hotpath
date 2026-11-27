// Tiny "--key value" / "--flag" command-line parser.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace hp {

class Args {
public:
    Args(int argc, char** argv) {
        for (int i = 1; i < argc; ++i) {
            std::string k = argv[i];
            if (k.rfind("--", 0) != 0) {
                positional_ = k;
                continue;
            }
            k = k.substr(2);
            if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) kv_[k] = argv[++i];
            else kv_[k] = "";
        }
    }
    bool has(const std::string& k) const { return kv_.count(k) != 0; }
    std::string str(const std::string& k, const std::string& def) const {
        auto it = kv_.find(k);
        return it == kv_.end() ? def : it->second;
    }
    uint64_t u64(const std::string& k, uint64_t def) const {
        auto it = kv_.find(k);
        return it == kv_.end() || it->second.empty() ? def : std::strtoull(it->second.c_str(), nullptr, 10);
    }
    int64_t i64(const std::string& k, int64_t def) const {
        auto it = kv_.find(k);
        return it == kv_.end() || it->second.empty() ? def : std::strtoll(it->second.c_str(), nullptr, 10);
    }
    double f64(const std::string& k, double def) const {
        auto it = kv_.find(k);
        return it == kv_.end() || it->second.empty() ? def : std::strtod(it->second.c_str(), nullptr);
    }
    const std::string& positional() const { return positional_; }

private:
    std::map<std::string, std::string> kv_;
    std::string positional_;
};

}  // namespace hp
