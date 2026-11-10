// Offline tests for the delay picker (no game needed):
//   g++ -std=c++20 -I src tests/decide_test.cpp -o /tmp/decide_test && /tmp/decide_test
#include "Decide.hpp"

#include <cstdio>
#include <functional>
#include <random>

using namespace pb::decide;

static int g_failed = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAILED line %d: %s\n", __LINE__, #cond);                \
            g_failed++;                                                          \
        }                                                                        \
    } while (0)

// A ghost that lives the whole horizon when `safe(total delay)` and dies after 5 steps otherwise.
struct Fake {
    std::function<bool(double)> safe;
    float remainder = 0.f;
    double deadAfter = 1e9; // waiting this long (or more) dies before the input
    int runs = 0;
    Outcome operator()(double delay, int horizon, int tap) {
        runs++;
        Outcome o;
        double total = delay + remainder;
        if (total >= deadAfter) {
            o.diedBeforeInput = true;
            return o;
        }
        o.survived = safe(total) && (tap == 0 || tapSafe(tap)) ? horizon : 5;
        return o;
    }
    std::function<bool(int)> tapSafe = [](int) { return true; };
};

static Params base(int limit) {
    Params p;
    p.smart = true;
    p.horizon = 64;
    p.limit = limit;
    p.base = 5;
    return p;
}

int main() {
    auto first = [](int) { return 0; };
    auto last = [](int count) { return count - 1; };

    { // a plain window: the smallest safe delay
        Fake f { [](double d) { return d >= 3 && d <= 6; } };
        auto c = choose(base(8), f, first);
        CHECK(c.ok && c.delay == 3.0 && c.result == 64 && c.sure);
    }
    { // finer timing: down to the quarter tick that works
        Fake f { [](double d) { return d >= 2.3; } };
        auto c = choose(base(8), f, first);
        CHECK(c.ok && c.delay == 2.5);
    }
    { // over the limit: no fix, but it says how much it would have taken
        Fake f { [](double d) { return d >= 9; } };
        auto c = choose(base(4), f, first);
        CHECK(!c.ok && c.needed == 9 && c.why == Why::NothingSafe);
    }
    { // the rest of a split step is part of the delay
        Fake f { [](double d) { return d >= 1.2; } };
        f.remainder = 0.5f;
        auto p = base(4);
        p.remainder = 0.5f;
        auto c = choose(p, f, first);
        CHECK(c.ok && c.delay == 0.75 && c.ticks() == 1.25);
    }
    { // only works on a knife edge: left alone
        Fake f { [](double d) { return d >= 3 && d <= 3.1; } };
        auto c = choose(base(8), f, first);
        CHECK(!c.ok && c.why == Why::KnifeEdge);
    }
    { // waiting only dies sooner: nothing to do, nothing "needed"
        Fake f { [](double) { return false; } };
        f.deadAfter = 2;
        auto c = choose(base(8), f, first);
        CHECK(!c.ok && c.needed == -1);
    }
    { // randomize: up to that many ticks above the smallest, never past the limit
        Fake f { [](double d) { return d >= 2; } };
        auto p = base(8);
        p.randomize = 3;
        auto c = choose(p, f, last);
        CHECK(c.ok && c.delay == 5.0 && c.random);
        p.limit = 3;
        Fake g { [](double d) { return d >= 2; } };
        c = choose(p, g, last);
        CHECK(c.ok && c.delay == 3.0);
    }
    { // has to work with your longer taps too
        Fake f { [](double d) { return d >= 2; } };
        f.tapSafe = [](int tap) { return tap < 10; };
        auto p = base(8);
        p.longTap = 14;
        auto c = choose(p, f, first);
        CHECK(!c.ok && c.why == Why::LongTap);
    }
    { // wave: the longer the wait, the more it has to gain
        Params p;
        p.wave = true;
        p.horizon = 38;
        p.limit = 6;
        p.base = 4;
        Outcome weak;
        weak.survived = 12;
        CHECK(!looseRule(p, 1, weak)); // needs base + 10
        weak.survived = 14;
        CHECK(looseRule(p, 1, weak));
        CHECK(!looseRule(p, 4, weak)); // 3 more ticks of waiting: needs 6 more
    }

    // Random windows: whatever it picks is within the limit and really safe.
    std::mt19937 gen(12345);
    for (int i = 0; i < 20000; ++i) {
        double lo = std::uniform_real_distribution<double>(0.0, 14.0)(gen);
        double width = std::uniform_real_distribution<double>(0.0, 6.0)(gen);
        float rem = i % 3 == 0 ? std::uniform_real_distribution<float>(0.05f, 0.95f)(gen) : 0.f;
        Fake f { [=](double d) { return d >= lo && d <= lo + width; } };
        f.remainder = rem;
        auto p = base(1 + static_cast<int>(gen() % 12));
        p.remainder = rem;
        p.randomize = static_cast<int>(gen() % 4);
        p.confidence = gen() % 4 != 0;
        p.finer = gen() % 4 != 0;
        p.bestFix = gen() % 2 == 0;
        auto c = choose(p, f, [&](int count) { return static_cast<int>(gen() % count); });
        if (c.ok) {
            CHECK(c.ticks() <= p.limit + 1e-6);
            CHECK(c.ticks() >= lo - 1e-6 && c.ticks() <= lo + width + 1e-6);
            if (p.confidence) CHECK(c.ticks() + 0.25 <= lo + width + 1e-6);
        }
        else if (c.needed > 0) {
            CHECK(c.needed > p.limit - 1 && c.needed >= lo - 1e-6);
        }
        CHECK(f.runs <= 80);
    }

    if (g_failed) {
        std::printf("%d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("all decide tests passed\n");
    return 0;
}
