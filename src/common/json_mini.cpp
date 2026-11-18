#include "json_mini.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace {
struct Parser {
    const char* p; const char* e; bool ok = true;
    void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++; }
    bool lit(const char* s) {
        const char* q = p;
        while (*s) { if (q >= e || *q != *s) return false; q++; s++; }
        p = q;
        return true;
    }
    static void put_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }
    bool hex4(unsigned& v) {
        if (e - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; i++) {
            char c = p[i]; v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        p += 4; return true;
    }
    std::string str() {
        std::string out; p++; // "
        while (p < e && *p != '"') {
            if (*p == '\\') {
                p++; if (p >= e) { ok = false; return out; }
                char c = *p++;
                switch (c) {
                case 'n': out += '\n'; break; case 't': out += '\t'; break;
                case 'r': out += '\r'; break; case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    unsigned v; if (!hex4(v)) { ok = false; return out; }
                    if (v >= 0xD800 && v < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        p += 2; unsigned lo; if (!hex4(lo)) { ok = false; return out; }
                        v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    put_utf8(out, v); break;
                }
                default: out += c;
                }
            } else out += *p++;
        }
        if (p < e) p++; else ok = false;
        return out;
    }
    Json val(int depth) {
        Json j; ws();
        if (p >= e || depth > 64) { ok = false; return j; }
        char c = *p;
        if (c == '{') {
            j.t = Json::Obj; p++; ws();
            if (p < e && *p == '}') { p++; return j; }
            while (ok) {
                ws(); if (p >= e || *p != '"') { ok = false; break; }
                std::string k = str(); ws();
                if (p >= e || *p != ':') { ok = false; break; }
                p++;
                j.o.emplace_back(k, val(depth + 1)); ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == '}') { p++; break; }
                ok = false;
            }
        } else if (c == '[') {
            j.t = Json::Arr; p++; ws();
            if (p < e && *p == ']') { p++; return j; }
            while (ok) {
                j.a.push_back(val(depth + 1)); ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == ']') { p++; break; }
                ok = false;
            }
        } else if (c == '"') {
            j.t = Json::Str; j.s = str();
        } else if (lit("true")) { j.t = Json::Bool; j.b = true; }
        else if (lit("false")) { j.t = Json::Bool; j.b = false; }
        else if (lit("null")) { j.t = Json::Null; }
        else {
            char* end = nullptr;
            std::string tmp(p, (size_t)((e - p) > 64 ? 64 : (e - p)));
            double v = strtod(tmp.c_str(), &end);
            if (end == tmp.c_str()) { ok = false; return j; }
            p += (end - tmp.c_str());
            j.t = Json::Num; j.n = v;
        }
        return j;
    }
};

void esc(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break; case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break; case '\r': out += "\\r"; break; case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
            else out += (char)c;
        }
    }
    out += '"';
}

void dumpv(const Json& j, std::string& out, int indent, int lv) {
    auto nl = [&](int l) { if (indent) { out += '\n'; out.append((size_t)(l * indent), ' '); } };
    switch (j.t) {
    case Json::Null: out += "null"; break;
    case Json::Bool: out += j.b ? "true" : "false"; break;
    case Json::Num: {
        char b[64];
        if (std::fabs(j.n) < 1e15 && j.n == (double)(long long)j.n) snprintf(b, sizeof b, "%lld", (long long)j.n);
        else snprintf(b, sizeof b, "%.17g", j.n);
        out += b; break;
    }
    case Json::Str: esc(out, j.s); break;
    case Json::Arr:
        out += '[';
        for (size_t i = 0; i < j.a.size(); i++) { if (i) out += ','; nl(lv + 1); dumpv(j.a[i], out, indent, lv + 1); }
        if (!j.a.empty()) nl(lv);
        out += ']'; break;
    case Json::Obj:
        out += '{';
        for (size_t i = 0; i < j.o.size(); i++) {
            if (i) out += ',';
            nl(lv + 1); esc(out, j.o[i].first); out += indent ? ": " : ":";
            dumpv(j.o[i].second, out, indent, lv + 1);
        }
        if (!j.o.empty()) nl(lv);
        out += '}'; break;
    }
}
} // namespace

Json Json::parse(const std::string& text, bool* ok) {
    Parser ps; ps.p = text.data(); ps.e = text.data() + text.size();
    // UTF-8 BOM を読み飛ばす
    if (ps.e - ps.p >= 3 && (unsigned char)ps.p[0] == 0xEF && (unsigned char)ps.p[1] == 0xBB && (unsigned char)ps.p[2] == 0xBF) ps.p += 3;
    Json j = ps.val(0);
    if (ok) *ok = ps.ok;
    return ps.ok ? j : Json();
}

std::string Json::dump(int indent) const {
    std::string out; dumpv(*this, out, indent, 0); return out;
}
