#pragma once
#include <string>
#include <iostream>

enum class TraceLevel { NONE, STEPS, CALC };

class Trace {
public:
    TraceLevel level = TraceLevel::NONE;

    bool show_steps() const { return level >= TraceLevel::STEPS; }
    bool show_calc()  const { return level >= TraceLevel::CALC; }

    void step(const std::string& msg, int depth = 0) const {
        if (show_steps()) print(depth, msg);
    }
    void calc(const std::string& msg, int depth = 0) const {
        if (show_calc()) print(depth, msg);
    }

private:
    static void print(int depth, const std::string& msg) {
        for (int i = 0; i < depth; i++) std::cerr << "  ";
        std::cerr << msg << '\n';
    }
};
