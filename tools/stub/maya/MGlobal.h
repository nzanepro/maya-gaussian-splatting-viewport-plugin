// Minimal stand-in so PlyLoader can be exercised outside Maya. Only the
// logging surface it actually touches is provided.
#pragma once
#include <string>
#include <iostream>
struct MString {
    std::string s;
    MString() {}
    MString(const char* c) : s(c ? c : "") {}
    MString operator+(const char* c)  const { MString r; r.s = s + (c?c:""); return r; }
    MString operator+(int v)          const { MString r; r.s = s + std::to_string(v); return r; }
    MString operator+(double v)       const { MString r; r.s = s + std::to_string(v); return r; }
    MString operator+(const MString& o)const{ MString r; r.s = s + o.s; return r; }
    const char* asChar() const { return s.c_str(); }
};
struct MGlobal {
    static void displayInfo(const MString& m)    { std::cout << "[info] "  << m.s << "\n"; }
    static void displayWarning(const MString& m) { std::cout << "[warn] "  << m.s << "\n"; }
    static void displayError(const MString& m)   { std::cout << "[error] " << m.s << "\n"; }
};
