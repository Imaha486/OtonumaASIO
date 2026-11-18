// json_mini.h - 設定ファイル用の小さな JSON 実装 (UTF-8)
#pragma once
#include <string>
#include <vector>
#include <utility>

struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj };
    Type t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    Json() {}
    static Json boolean(bool v) { Json j; j.t = Bool; j.b = v; return j; }
    static Json number(double v) { Json j; j.t = Num; j.n = v; return j; }
    static Json string(const std::string& v) { Json j; j.t = Str; j.s = v; return j; }
    static Json object() { Json j; j.t = Obj; return j; }
    static Json array() { Json j; j.t = Arr; return j; }

    const Json* get(const std::string& k) const {
        if (t != Obj) return nullptr;
        for (auto& p : o) if (p.first == k) return &p.second;
        return nullptr;
    }
    Json* get(const std::string& k) {
        if (t != Obj) return nullptr;
        for (auto& p : o) if (p.first == k) return &p.second;
        return nullptr;
    }
    Json& set(const std::string& k, const Json& v) {
        if (t != Obj) { t = Obj; o.clear(); }
        for (auto& p : o) if (p.first == k) { p.second = v; return p.second; }
        o.emplace_back(k, v);
        return o.back().second;
    }
    bool erase(const std::string& k) {
        for (size_t i = 0; i < o.size(); i++) if (o[i].first == k) { o.erase(o.begin() + i); return true; }
        return false;
    }
    double num(double def = 0) const { return t == Num ? n : (t == Bool ? (b ? 1 : 0) : def); }
    std::string str(const std::string& def = "") const { return t == Str ? s : def; }

    static Json parse(const std::string& text, bool* ok = nullptr);
    std::string dump(int indent = 0) const;
};
