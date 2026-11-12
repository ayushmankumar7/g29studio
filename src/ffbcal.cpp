#include "ffbcal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

using clk = std::chrono::steady_clock;

namespace {

long long msSince(clk::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count();
}

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

double elapsedMs(clk::time_point t0) {
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

} // namespace

FfbCalibrator::~FfbCalibrator() {
    cancel();
    join();
}

void FfbCalibrator::join() {
    if (m_thread.joinable()) m_thread.join();
}

void FfbCalibrator::cancel() {
    m_cancel.store(true);
}

void FfbCalibrator::setMessage(const std::string& s) {
    std::lock_guard<std::mutex> lk(m_mx);
    m_message = s;
}

std::string FfbCalibrator::message() const {
    std::lock_guard<std::mutex> lk(m_mx);
    return m_message;
}

std::vector<CalPoint> FfbCalibrator::points() const {
    std::lock_guard<std::mutex> lk(m_mx);
    return m_points;
}

std::vector<std::pair<float, float>> FfbCalibrator::lut() const {
    std::lock_guard<std::mutex> lk(m_mx);
    return m_lut;
}

bool FfbCalibrator::start(Wheel* w, const Params& p, std::string& err) {
    if (m_running.load()) { err = "Calibration is already running."; return false; }
    if (!w || !w->isOpen()) { err = "Connect a wheel first."; return false; }
    if (!w->ffbReady())     { err = "Force feedback is not available on this device."; return false; }

    join();                    // reap any previous thread
    m_cancel.store(false);
    m_failed.store(false);
    m_finished.store(false);
    m_progress.store(0.0f);
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_points.clear();
        m_lut.clear();
        m_message = "Starting...";
    }
    m_running.store(true);
    m_thread = std::thread(&FfbCalibrator::run, this, w, p);
    return true;
}

void FfbCalibrator::run(Wheel* w, Params p) {
    p.steps     = std::clamp(p.steps, 5, 80);
    p.reps      = std::clamp(p.reps, 1, 6);
    p.distance  = std::clamp(p.distance, 100, 6000);
    p.timeoutMs = std::clamp(p.timeoutMs, 200, 3000);

    // Condition effects would fight the measurement, so silence them and
    // restore nothing here -- the UI reapplies the user's settings afterwards.
    w->setSpring(0.0f, 1.0f);
    w->setDamper(0.0f);
    w->setConstantForce(0.0f);

    LONG center = 32767;
    for (const auto& a : w->axes())
        if (a.offset == DIJOFS_X) { center = (a.min + a.max) / 2; break; }

    auto readX = [&]() -> LONG {
        w->poll();
        return Wheel::axisRaw(w->state(), DIJOFS_X);
    };

    // Drive the wheel gently back toward centre so every sample starts from a
    // comparable place and never reaches the end stop.
    auto recenter = [&](int timeoutMs) {
        const auto t0 = clk::now();
        while (msSince(t0) < timeoutMs && !m_cancel.load()) {
            const LONG x = readX();
            const float e = float(x - center) / 32768.0f;
            if (std::fabs(e) < 0.02f) break;
            float mag = -e * 0.55f;
            if (mag > 0.0f && mag < 0.13f) mag = 0.13f;   // beat static friction
            if (mag < 0.0f && mag > -0.13f) mag = -0.13f;
            w->setConstantForce(std::clamp(mag, -0.55f, 0.55f));
            sleepMs(4);
        }
        w->setConstantForce(0.0f);
        sleepMs(320);
    };

    // A geared wheel coasts after force is released; residual velocity would
    // corrupt the next timing, so wait for it to genuinely stop.
    auto waitUntilStill = [&](int maxMs) {
        const auto t0 = clk::now();
        LONG prev = 0;
        bool have = false;
        int quiet = 0;
        while (msSince(t0) < maxMs && !m_cancel.load()) {
            const LONG x = readX();
            if (have && std::labs(x - prev) < 25) { if (++quiet >= 3) return; }
            else quiet = 0;
            prev = x;
            have = true;
            sleepMs(50);
        }
    };

    // Milliseconds to travel `distance` counts, or -1 if it never does.
    auto timeToMove = [&](float mag, int dir) -> double {
        recenter(3000);
        waitUntilStill(1500);
        if (m_cancel.load()) return -1.0;

        const LONG x0 = readX();
        w->setConstantForce(mag * dir);
        const auto t0 = clk::now();
        double result = -1.0;
        while (!m_cancel.load()) {
            const double el = elapsedMs(t0);
            if (el > (double)p.timeoutMs) break;
            if (std::labs(readX() - x0) >= p.distance) { result = el; break; }
            sleepMs(1);
        }
        w->setConstantForce(0.0f);
        return result;
    };

    std::vector<CalPoint> pts;
    bool anyMovement = false;

    for (int i = 1; i <= p.steps && !m_cancel.load(); ++i) {
        const float f = (float)i / (float)p.steps;

        std::vector<double> samples;
        for (int r = 0; r < p.reps && !m_cancel.load(); ++r) {
            // Average both directions: cancels gear preload, mounting bias and
            // any leftover momentum, which otherwise alternates step to step.
            double sum = 0.0;
            for (int dir : {+1, -1}) {
                const double t = timeToMove(f, dir);
                if (t > 0.0) { sum += 1.0 / (t * t); anyMovement = true; }
            }
            samples.push_back(sum * 0.5);
        }
        if (samples.empty()) break;
        std::sort(samples.begin(), samples.end());
        const double med = samples[samples.size() / 2];

        pts.push_back(CalPoint{f, (float)med});
        m_progress.store((float)i / (float)p.steps);

        char buf[160];
        snprintf(buf, sizeof(buf), "Measuring %d/%d   force %.2f -> torque %.4f",
                 i, p.steps, f, med * 1000.0);
        setMessage(buf);
        {
            std::lock_guard<std::mutex> lk(m_mx);
            m_points = pts;
        }
    }

    w->setConstantForce(0.0f);

    if (m_cancel.load()) {
        setMessage("Cancelled.");
        m_failed.store(true);
        m_running.store(false);
        m_finished.store(true);
        return;
    }
    if (pts.empty() || !anyMovement) {
        setMessage("The wheel never moved at any force level. Check that force "
                   "feedback is enabled in G HUB and that nothing is holding the wheel.");
        m_failed.store(true);
        m_running.store(false);
        m_finished.store(true);
        return;
    }

    // Normalise, then force monotonicity -- measurement noise can dip.
    double maxR = 0.0;
    for (const auto& q : pts) maxR = std::max(maxR, (double)q.response);
    if (maxR <= 0.0) {
        setMessage("No usable response measured.");
        m_failed.store(true);
        m_running.store(false);
        m_finished.store(true);
        return;
    }
    std::vector<float> xs, ys;
    float run = 0.0f;
    for (auto& q : pts) {
        q.response = (float)std::clamp(q.response / maxR, 0.0, 1.0);
        run = std::max(run, q.response);
        xs.push_back(q.force);
        ys.push_back(run);
    }

    // Invert: for each requested output r, find the input x whose measured
    // response equals r. That is the correction curve.
    std::vector<float> cx{0.0f}, cy{0.0f};
    for (size_t i = 0; i < xs.size(); ++i) { cx.push_back(xs[i]); cy.push_back(ys[i]); }

    std::vector<std::pair<float, float>> out;
    out.emplace_back(0.0f, 0.0f);
    for (int j = 1; j < p.steps; ++j) {
        const float r = (float)j / (float)p.steps;
        float x = 1.0f;
        for (size_t i = 1; i < cy.size(); ++i) {
            if (cy[i] >= r) {
                const float y0 = cy[i - 1], y1 = cy[i];
                const float x0 = cx[i - 1], x1 = cx[i];
                x = (y1 - y0) > 1e-9f ? x0 + (r - y0) * (x1 - x0) / (y1 - y0) : x1;
                break;
            }
        }
        out.emplace_back(r, std::clamp(x, 0.0f, 1.0f));
    }
    out.emplace_back(1.0f, 1.0f);

    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_points = pts;
        m_lut = out;
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "Done. %d-point curve; dead zone ends near %.0f%% force.",
             (int)out.size(), out.size() > 1 ? out[1].second * 100.0f : 0.0f);
    setMessage(buf);

    m_running.store(false);
    m_finished.store(true);
}

// ---------------------------------------------------------------------------

bool saveLutFile(const std::string& path,
                 const std::vector<std::pair<float, float>>& lut,
                 std::string& err) {
    if (lut.empty()) { err = "Nothing to save."; return false; }
    std::ofstream o(path);
    if (!o) { err = "Cannot write " + path; return false; }
    for (const auto& [in, out] : lut) {
        char line[64];
        snprintf(line, sizeof(line), "%.4f|%.4f\n", in, out);
        o << line;
    }
    return true;
}

bool loadLutFile(const std::string& path,
                 std::vector<std::pair<float, float>>& out,
                 std::string& err) {
    std::ifstream in(path);
    if (!in) { err = "Cannot read " + path; return false; }
    std::vector<std::pair<float, float>> v;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const auto bar = line.find('|');
        if (bar == std::string::npos) continue;
        v.emplace_back((float)std::atof(line.substr(0, bar).c_str()),
                       (float)std::atof(line.substr(bar + 1).c_str()));
    }
    if (v.size() < 2) { err = "That file does not look like a LUT."; return false; }
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    out = std::move(v);
    return true;
}
