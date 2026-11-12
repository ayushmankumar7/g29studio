#pragma once

#include "wheel.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// One measured sample: how much real torque the wheel produced for a given
// requested force.
struct CalPoint {
    float force    = 0.0f;   // what we asked for, 0..1
    float response = 0.0f;   // what we measured, normalised 0..1
};

// Measures a wheel's true force response and inverts it into a lookup table.
//
// Method: apply a constant force from rest and time how long the wheel takes to
// travel a fixed, small distance. Under constant torque from rest the distance
// follows s = at^2/2, so torque is proportional to 1/t^2.
//
// Timing a short fixed distance rather than measuring distance in a fixed time
// is essential on a geared wheel: a G29 leaves its acceleration phase almost at
// once and hits a velocity limit, after which displacement stops tracking
// torque and the measured curve flattens into nonsense.
//
// Runs on its own thread. While it is running the caller must not touch the
// Wheel, because DirectInput calls are not safe to make concurrently.
class FfbCalibrator {
public:
    struct Params {
        int steps     = 20;    // force levels sampled between 0 and full
        int reps      = 2;     // repeats per level (median is taken)
        int distance  = 400;   // axis counts to travel; keep small
        int timeoutMs = 900;   // give up if the wheel never gets there
    };

    FfbCalibrator() = default;
    ~FfbCalibrator();
    FfbCalibrator(const FfbCalibrator&) = delete;
    FfbCalibrator& operator=(const FfbCalibrator&) = delete;

    bool start(Wheel* w, const Params& p, std::string& err);
    void cancel();
    void join();

    bool running()  const { return m_running.load(); }
    bool finished() const { return m_finished.load(); }
    bool failed()   const { return m_failed.load(); }
    float progress() const { return m_progress.load(); }   // 0..1

    std::string message() const;
    std::vector<CalPoint> points() const;
    std::vector<std::pair<float, float>> lut() const;

    void acknowledge() { m_finished.store(false); }

private:
    void run(Wheel* w, Params p);
    void setMessage(const std::string& s);

    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_finished{false};
    std::atomic<bool> m_failed{false};
    std::atomic<bool> m_cancel{false};
    std::atomic<float> m_progress{0.0f};

    mutable std::mutex m_mx;
    std::string m_message;
    std::vector<CalPoint> m_points;
    std::vector<std::pair<float, float>> m_lut;
};

// Writes an Assetto Corsa format LUT (also what Manual Transmission reads).
bool saveLutFile(const std::string& path,
                 const std::vector<std::pair<float, float>>& lut,
                 std::string& err);
bool loadLutFile(const std::string& path,
                 std::vector<std::pair<float, float>>& out,
                 std::string& err);
