#include "profile.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

float shape(float v, float deadzone, float gamma) {
    const float s = v < 0.0f ? -1.0f : 1.0f;
    float a = std::fabs(v);
    if (a <= deadzone) return 0.0f;
    // Rescale so motion resumes smoothly from the edge of the dead zone
    // instead of jumping.
    a = (a - deadzone) / std::max(1e-6f, 1.0f - deadzone);
    a = std::pow(std::clamp(a, 0.0f, 1.0f), std::max(0.05f, gamma));
    return s * a;
}

float FfbCfg::applyLut(float n) const {
    if (!useLut || lut.size() < 2) return n;
    const float s = n < 0.0f ? -1.0f : 1.0f;
    const float a = std::clamp(std::fabs(n), 0.0f, 1.0f);

    // Piecewise-linear lookup on the input column.
    if (a <= lut.front().first)  return s * lut.front().second;
    if (a >= lut.back().first)   return s * lut.back().second;
    for (size_t i = 1; i < lut.size(); ++i) {
        if (lut[i].first >= a) {
            const float x0 = lut[i - 1].first,  x1 = lut[i].first;
            const float y0 = lut[i - 1].second, y1 = lut[i].second;
            const float t = (x1 - x0) > 1e-9f ? (a - x0) / (x1 - x0) : 0.0f;
            return s * std::clamp(y0 + t * (y1 - y0), 0.0f, 1.0f);
        }
    }
    return n;
}

float AxisCal::bipolar(LONG raw) const {
    const float c  = (float)rawCenter;
    float v;
    if (raw >= rawCenter) {
        const float span = std::max(1.0f, (float)rawMax - c);
        v = ((float)raw - c) / span;
    } else {
        const float span = std::max(1.0f, c - (float)rawMin);
        v = ((float)raw - c) / span;
    }
    v = std::clamp(v, -1.0f, 1.0f);
    if (invert) v = -v;
    return shape(v, deadzone, gamma);
}

float AxisCal::unipolar(LONG raw) const {
    const float span = std::max(1.0f, (float)(rawMax - rawMin));
    float v = ((float)raw - (float)rawMin) / span;
    v = std::clamp(v, 0.0f, 1.0f);
    if (invert) v = 1.0f - v;
    return shape(v, deadzone, gamma);
}

// ---------------------------------------------------------------------------
// Persistence: a plain key=value file. No dependency, and editable by hand.

namespace {

void writeAxis(std::ostream& o, const char* p, const AxisCal& a) {
    o << p << ".offset="    << a.offset    << "\n"
      << p << ".rawMin="    << a.rawMin    << "\n"
      << p << ".rawMax="    << a.rawMax    << "\n"
      << p << ".rawCenter=" << a.rawCenter << "\n"
      << p << ".invert="    << (a.invert ? 1 : 0) << "\n"
      << p << ".deadzone="  << a.deadzone  << "\n"
      << p << ".gamma="     << a.gamma     << "\n";
}

using KV = std::map<std::string, std::string>;

long   gi(const KV& m, const std::string& k, long d) {
    auto it = m.find(k); return it == m.end() ? d : std::strtol(it->second.c_str(), nullptr, 10);
}
double gf(const KV& m, const std::string& k, double d) {
    auto it = m.find(k); return it == m.end() ? d : std::strtod(it->second.c_str(), nullptr);
}

void readAxis(const KV& m, const char* p, AxisCal& a) {
    const std::string s(p);
    a.offset    = (DWORD)gi(m, s + ".offset",    (long)a.offset);
    a.rawMin    = (LONG) gi(m, s + ".rawMin",    a.rawMin);
    a.rawMax    = (LONG) gi(m, s + ".rawMax",    a.rawMax);
    a.rawCenter = (LONG) gi(m, s + ".rawCenter", a.rawCenter);
    a.invert    =        gi(m, s + ".invert",    a.invert ? 1 : 0) != 0;
    a.deadzone  = (float)gf(m, s + ".deadzone",  a.deadzone);
    a.gamma     = (float)gf(m, s + ".gamma",     a.gamma);
}

} // namespace

bool Profile::save(const std::string& path, std::string& err) const {
    std::ofstream o(path);
    if (!o) { err = "Cannot write " + path; return false; }

    o << "# g29studio profile\n";
    o << "name=" << name << "\n";
    o << "steeringRangeDeg=" << steeringRangeDeg << "\n";

    writeAxis(o, "steering", steering);
    writeAxis(o, "throttle", throttle);
    writeAxis(o, "brake",    brake);
    writeAxis(o, "clutch",   clutch);

    o << "gearbox.enabled="      << (gearbox.enabled ? 1 : 0) << "\n"
      << "gearbox.gearCount="    << gearbox.gearCount         << "\n"
      << "gearbox.stallEnabled=" << (gearbox.stallEnabled ? 1 : 0) << "\n"
      << "gearbox.clutchBite="   << gearbox.clutchBite        << "\n"
      << "gearbox.neutralCuts="  << (gearbox.neutralCutsThrottle ? 1 : 0) << "\n";
    for (int i = 0; i < 7; ++i) {
        o << "gearbox.button" << i << "=" << gearbox.gearButton[i] << "\n";
        o << "gearbox.ratio"  << i << "=" << gearbox.gearRatio[i]  << "\n";
    }

    o << "ffb.masterGain="       << ffb.masterGain       << "\n"
      << "ffb.springGain="       << ffb.springGain       << "\n"
      << "ffb.springSaturation=" << ffb.springSaturation << "\n"
      << "ffb.damperGain="       << ffb.damperGain       << "\n"
      << "ffb.satScale="         << ffb.satScale         << "\n"
      << "ffb.detailGain="       << ffb.detailGain       << "\n"
      << "ffb.invertForce="      << (ffb.invertForce ? 1 : 0) << "\n"
      << "ffb.minForce="         << ffb.minForce         << "\n"
      << "ffb.useLut="           << (ffb.useLut ? 1 : 0) << "\n"
      << "ffb.lutFile="          << ffb.lutFile          << "\n";

    o << "bridge.enabled="       << (bridge.enabled ? 1 : 0)       << "\n"
      << "bridge.steerScale="    << bridge.steerScale              << "\n"
      << "bridge.shapeThrottle=" << (bridge.shapeThrottle ? 1 : 0) << "\n"
      << "bridge.rumbleToWheel=" << bridge.rumbleToWheel           << "\n"
      << "bridge.mapButtons="    << (bridge.mapButtons ? 1 : 0)    << "\n";
    return true;
}

bool Profile::load(const std::string& path, std::string& err) {
    std::ifstream in(path);
    if (!in) { err = "Cannot read " + path; return false; }

    KV m;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
        m[k] = v;
    }

    if (auto it = m.find("name"); it != m.end()) name = it->second;
    steeringRangeDeg = (int)gi(m, "steeringRangeDeg", steeringRangeDeg);

    readAxis(m, "steering", steering);
    readAxis(m, "throttle", throttle);
    readAxis(m, "brake",    brake);
    readAxis(m, "clutch",   clutch);

    gearbox.enabled      = gi(m, "gearbox.enabled",      gearbox.enabled ? 1 : 0) != 0;
    gearbox.gearCount    = (int)gi(m, "gearbox.gearCount", gearbox.gearCount);
    gearbox.stallEnabled = gi(m, "gearbox.stallEnabled",  gearbox.stallEnabled ? 1 : 0) != 0;
    gearbox.clutchBite   = (float)gf(m, "gearbox.clutchBite", gearbox.clutchBite);
    gearbox.neutralCutsThrottle =
        gi(m, "gearbox.neutralCuts", gearbox.neutralCutsThrottle ? 1 : 0) != 0;
    for (int i = 0; i < 7; ++i) {
        gearbox.gearButton[i] = (int)gi(m, "gearbox.button" + std::to_string(i),
                                        gearbox.gearButton[i]);
        gearbox.gearRatio[i]  = (float)gf(m, "gearbox.ratio" + std::to_string(i),
                                          gearbox.gearRatio[i]);
    }

    ffb.masterGain       = (float)gf(m, "ffb.masterGain",       ffb.masterGain);
    ffb.springGain       = (float)gf(m, "ffb.springGain",       ffb.springGain);
    ffb.springSaturation = (float)gf(m, "ffb.springSaturation", ffb.springSaturation);
    ffb.damperGain       = (float)gf(m, "ffb.damperGain",       ffb.damperGain);
    ffb.satScale         = (float)gf(m, "ffb.satScale",         ffb.satScale);
    ffb.detailGain       = (float)gf(m, "ffb.detailGain",       ffb.detailGain);
    ffb.invertForce      = gi(m, "ffb.invertForce", ffb.invertForce ? 1 : 0) != 0;
    ffb.minForce         = (float)gf(m, "ffb.minForce",         ffb.minForce);
    ffb.useLut           = gi(m, "ffb.useLut", ffb.useLut ? 1 : 0) != 0;
    if (auto it = m.find("ffb.lutFile"); it != m.end()) ffb.lutFile = it->second;

    bridge.enabled       = gi(m, "bridge.enabled", bridge.enabled ? 1 : 0) != 0;
    bridge.steerScale    = (float)gf(m, "bridge.steerScale", bridge.steerScale);
    bridge.shapeThrottle = gi(m, "bridge.shapeThrottle", bridge.shapeThrottle ? 1 : 0) != 0;
    bridge.rumbleToWheel = (float)gf(m, "bridge.rumbleToWheel", bridge.rumbleToWheel);
    bridge.mapButtons    = gi(m, "bridge.mapButtons", bridge.mapButtons ? 1 : 0) != 0;
    return true;
}
