#pragma once

// Picks the delay for an input that would kill you as it is. No game code in
// here: the hidden ghost is passed in as a function, so this part can be tested
// on its own (see tests/).

#include <algorithm>
#include <cmath>
#include <vector>

namespace pb::decide {

constexpr int MAX_DELAY = 24; // largest delay the settings allow, in ticks

// What a ghost run with one delay ended with.
struct Outcome {
    int survived = 0;             // steps it lived after the input went in
    bool diedBeforeInput = false; // died while waiting for the input
    bool hitStop = false;         // reached something it can't use (unknown from there)
    bool cleared = false;         // jumped and landed safely before the end
};

struct Params {
    bool wave = false;     // wave: stricter the longer the wait
    bool smart = false;    // cube-like modes: the jump has to clear everything ahead or land
    bool dual = false;
    int horizon = 48;      // steps a run looks ahead
    int limit = 2;         // largest delay allowed, in ticks
    int base = 0;          // steps survived with no delay
    float remainder = 0.f; // part of the current step still to run (already part of the delay)
    int randomize = 0;     // pick any safe delay up to this many ticks above the smallest
    bool bestFix = true;   // compare the safe delays further ahead
    bool finer = true;     // quarter tick delays
    bool confidence = true; // a quarter tick later has to work too
    int longTap = 0;       // > 0: the fix also has to work with a tap this long
};

enum class Why { Fixed, NothingSafe, KnifeEdge, LongTap };

struct Choice {
    bool ok = false;       // false = nothing safe, let the input through
    double delay = 0.0;    // whole steps after the rest of the current step (+ a fraction)
    float remainder = 0.f; // the rest of the current step (part of the real delay)
    int base = 0;          // steps survived with no delay
    int result = 0;        // steps survived with the chosen delay
    bool random = false;
    bool loose = false;    // only passed the looser rule
    bool better = false;   // best fix picked a later delay that lasts longer
    bool sure = false;     // passed the confidence check
    Why why = Why::NothingSafe;
    // No fix within the limit: the smallest delay past it that would have worked,
    // in ticks (how early the input was). -1 = none up to MAX_DELAY.
    int needed = -1;

    double ticks() const { return delay + remainder; }
};

// The original rule: the delayed input has to live clearly longer.
inline bool looseRule(Params const& p, double delay, Outcome const& r) {
    if (r.hitStop) return r.survived > 0 && r.survived >= p.base;
    if (p.wave) {
        double wait = std::max(1.0, delay + p.remainder);
        int extra = static_cast<int>(std::ceil(2 * (wait - 1)));
        int need = p.base + 10 + extra;
        int gain = 6 + extra;
        return r.survived < p.horizon ? r.survived >= need : (r.survived - p.base >= gain || r.survived >= need);
    }
    return r.survived >= std::min(p.base + 12, p.horizon);
}

// Smart safety: live through everything the run can see.
inline bool strictRule(Params const& p, double delay, Outcome const& r) {
    if (!p.smart) return looseRule(p, delay, r);
    if (r.hitStop) return r.survived > 0 && r.survived >= p.base;
    return r.survived >= p.horizon;
}

// sim(delay, horizon, tap) -> Outcome   (tap 0 = your usual tap, > 0 = a tap that long)
// pick(count) -> index below count      (randomize)
template <class Sim, class Pick>
Choice choose(Params const& p, Sim&& sim, Pick&& pick) {
    bool strictOk[MAX_DELAY + 1] {};
    bool looseOk[MAX_DELAY + 1] {};
    bool tested[MAX_DELAY + 1] {};
    int survived[MAX_DELAY + 1] {};
    int firstStrict = -1;
    int firstLoose = -1;
    int window = std::max(p.randomize, p.bestFix ? 2 : 0);
    // The real delay is the rest of the current step plus `d` whole steps. Mid-step
    // (Click Between Frames), firing right after the rest of this step is already a delay.
    float rem = p.remainder;
    int dmin = rem > 0.f ? 0 : 1;
    int dmax = std::clamp(static_cast<int>(std::floor(p.limit - rem + 1e-6)), 0, MAX_DELAY);
    bool deadEnd = false; // waiting any longer only dies sooner
    for (int d = dmin; d <= dmax; ++d) {
        Outcome r = sim(static_cast<double>(d), p.horizon, 0);
        if (r.diedBeforeInput) {
            deadEnd = true;
            break;
        }
        tested[d] = true;
        survived[d] = r.survived;
        strictOk[d] = strictRule(p, d, r);
        looseOk[d] = looseRule(p, d, r);
        if (strictOk[d] && firstStrict < 0) firstStrict = d;
        if (looseOk[d] && firstLoose < 0) firstLoose = d;
        // with a safe delay found, only a few ticks above it still matter
        if (firstStrict >= 0 && d >= firstStrict + window) break;
    }

    Choice c;
    c.base = p.base;
    c.remainder = rem;
    bool useStrict = firstStrict >= 0;
    bool* pool = useStrict ? strictOk : looseOk;
    int first = useStrict ? firstStrict : firstLoose;
    if (first < 0) {
        // nothing within the limit: how much delay would it have taken?
        for (int d = std::max(dmax + 1, dmin); !deadEnd && d <= MAX_DELAY; ++d) {
            Outcome r = sim(static_cast<double>(d), p.horizon, 0);
            if (r.diedBeforeInput) break;
            if (strictRule(p, d, r) || looseRule(p, d, r)) {
                c.needed = static_cast<int>(std::ceil(d + rem - 1e-6));
                break;
            }
        }
        return c;
    }
    c.loose = !useStrict;
    auto passes = [&](double d, Outcome const& r) {
        return !r.diedBeforeInput && (useStrict ? strictRule(p, d, r) : looseRule(p, d, r));
    };

    // the safe delays close above the smallest one
    std::vector<int> candidates;
    for (int d = first; d <= std::min(dmax, first + window); ++d) {
        if (pool[d]) candidates.push_back(d);
    }

    // Best fix: look further ahead and keep only the delays that last longest
    // (surviving this jump just to die on the next one isn't a fix).
    std::vector<int> best = candidates;
    if (p.bestFix && candidates.size() > 1) {
        int deep = p.horizon + p.horizon / 2;
        std::vector<int> scores;
        int top = -1;
        for (int d : candidates) {
            Outcome r = sim(static_cast<double>(d), deep, 0);
            int score = r.cleared ? deep + 1 : r.hitStop ? deep : r.survived;
            scores.push_back(score);
            top = std::max(top, score);
        }
        best.clear();
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (scores[i] == top) best.push_back(candidates[i]);
        }
    }
    c.better = best.front() != first;

    // Randomize: any of the best delays from the smallest one up to `randomize` above it.
    int picked = best.front();
    if (p.randomize > 0) {
        std::vector<int> options;
        for (int d : best) {
            if (d <= best.front() + p.randomize) options.push_back(d);
        }
        if (options.size() > 1) {
            size_t i = static_cast<size_t>(pick(static_cast<int>(options.size())));
            picked = options[std::min(i, options.size() - 1)];
        }
    }
    c.random = picked != best.front();
    double chosen = picked;

    // Finer timing: when the tick before isn't safe, find the smallest safe delay
    // to a quarter of a tick, so your click moves only as much as it has to.
    if (p.finer && !p.dual && !c.random && picked >= 1) {
        // the step before is unsafe: either it was tested, or it's the input as it is (no delay)
        bool beforeUnsafe = picked - 1 < dmin || (tested[picked - 1] && !pool[picked - 1]);
        if (beforeUnsafe) {
            double lo = picked - 1;
            double hi = picked;
            for (int i = 0; i < 2; ++i) {
                double mid = (lo + hi) / 2;
                if (passes(mid, sim(mid, p.horizon, 0))) hi = mid;
                else lo = mid;
            }
            chosen = hi;
        }
    }

    // Confidence: a quarter tick later has to work too, so the fix isn't on a knife edge.
    // Your longer taps: if you sometimes hold clearly longer, the fix has to work for
    // those too. If a delay fails either, try the next safe ones; if none holds up,
    // leave the input alone.
    if (p.confidence || p.longTap > 0) {
        Why failed = Why::NothingSafe;
        auto verified = [&](double d) {
            if (p.confidence && !passes(d, sim(d + 0.25, p.horizon, 0))) {
                failed = Why::KnifeEdge;
                return false;
            }
            if (p.longTap > 0 && !passes(d, sim(d, p.horizon, p.longTap))) {
                failed = Why::LongTap;
                return false;
            }
            return true;
        };
        std::vector<double> order { chosen };
        if (chosen != picked) order.push_back(picked);
        for (int d : best) {
            if (d > picked && order.size() < 4) order.push_back(d);
        }
        for (int d : candidates) {
            if (d > picked && order.size() < 4
                && std::find(order.begin(), order.end(), static_cast<double>(d)) == order.end()) {
                order.push_back(d);
            }
        }
        bool found = false;
        for (double d : order) {
            if (d <= dmax && verified(d)) {
                chosen = d;
                found = true;
                break;
            }
        }
        if (!found) {
            Choice none;
            none.base = p.base;
            none.remainder = rem;
            none.why = failed;
            return none;
        }
        c.sure = p.confidence;
    }

    c.ok = true;
    c.why = Why::Fixed;
    c.delay = chosen;
    int whole = std::clamp(static_cast<int>(std::ceil(chosen - 1e-6)), 0, MAX_DELAY);
    c.result = survived[whole];
    return c;
}

} // namespace pb::decide
