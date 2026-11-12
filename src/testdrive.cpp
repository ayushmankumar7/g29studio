#include "testdrive.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace {

constexpr float kMass      = 1400.0f;   // kg
constexpr float kIzz       = 1900.0f;   // kg m^2
constexpr float kA         = 1.25f;     // CoG to front axle, m
constexpr float kB         = 1.35f;     // CoG to rear axle, m
constexpr float kMaxSteer  = 0.52f;     // radians at full lock (~30 deg)
constexpr float kCorner    = 95000.0f;  // cornering stiffness, N/rad
constexpr float kVmax      = 68.0f;     // m/s
constexpr float kBaseDrive = 11000.0f;  // N in first gear
constexpr float kBrakeMax  = 13000.0f;  // N
constexpr float kIdleRpm   = 850.0f;
constexpr float kMaxRpm    = 7200.0f;
constexpr float kStallRpm  = 550.0f;

float frand() { return (float)std::rand() / (float)RAND_MAX * 2.0f - 1.0f; }

// Saturating tyre: linear at small slip, flattening as it reaches the limit.
// The flattening is what makes a wheel go light when the front washes out.
float tyreLateral(float slip, float load, float grip) {
    const float peak = std::max(1.0f, grip * load);
    return -peak * std::tanh(kCorner * slip / peak);
}

} // namespace

float CarState::speedKph() const {
    return std::sqrt(vx * vx + vy * vy) * 3.6f;
}

void TestDrive::reset() {
    m_s = CarState{};
    m_s.x = 0.0f;
    m_s.y = -(kTrackRadiusY);
    m_s.heading = 0.0f;
    m_textureAccum = 0.0f;
    m_texture = 0.0f;
    m_sat = 0.0f;
}

float TestDrive::trackOffset(float x, float y) {
    // Distance from an elliptical centreline, approximated by normalising the
    // ellipse equation. Good enough for a test bench.
    const float nx = x / kTrackRadiusX;
    const float ny = y / kTrackRadiusY;
    const float r  = std::sqrt(nx * nx + ny * ny);
    const float avg = 0.5f * (kTrackRadiusX + kTrackRadiusY);
    return (r - 1.0f) * avg;
}

int TestDrive::surfaceAt(float x, float y, float* gripOut) {
    const float d = std::fabs(trackOffset(x, y));
    int surf;
    float grip;
    if (d <= kHalfWidth)                        { surf = SURF_ASPHALT; grip = 1.45f; }
    else if (d <= kHalfWidth + kKerbWidth)      { surf = SURF_KERB;    grip = 1.15f; }
    else                                        { surf = SURF_GRASS;   grip = 0.55f; }
    if (gripOut) *gripOut = grip;
    return surf;
}

void TestDrive::step(float dt, const CarInput& in, const GearboxCfg& gb) {
    dt = std::clamp(dt, 1.0f / 500.0f, 1.0f / 20.0f);

    float grip = 1.0f;
    m_s.surface = surfaceAt(m_s.x, m_s.y, &grip);
    m_s.grip    = grip;

    const float delta = std::clamp(in.steer, -1.0f, 1.0f) * kMaxSteer;

    // ---- Gearbox -----------------------------------------------------------
    int gear = gb.enabled ? in.gear : 1;
    const bool clutchIn = gb.enabled && (in.clutch > gb.clutchBite);
    const bool neutral  = gb.enabled && gear == 0;

    const int ratioIdx = std::clamp(std::abs(gear), 0, 6);
    const float ratio  = std::max(0.05f, gb.gearRatio[ratioIdx]);
    const float topSpeed = kVmax * ratio;

    const float fwdSpeed = m_s.vx;
    float rpmNorm = std::clamp(std::fabs(fwdSpeed) / std::max(1.0f, topSpeed), 0.0f, 1.25f);

    if (clutchIn || neutral) {
        // Disengaged: engine free-revs with throttle instead of following wheels.
        const float target = kIdleRpm + in.throttle * (kMaxRpm - kIdleRpm);
        m_s.engineRpm += (target - m_s.engineRpm) * std::min(1.0f, dt * 5.0f);
    } else {
        m_s.engineRpm = kIdleRpm + rpmNorm * (kMaxRpm - kIdleRpm);
    }

    if (gb.enabled && gb.stallEnabled) {
        if (!clutchIn && !neutral && m_s.engineRpm < kStallRpm) m_s.stalled = true;
        if (clutchIn || neutral) m_s.stalled = false;   // dip the clutch to recover
    } else {
        m_s.stalled = false;
    }

    // ---- Longitudinal ------------------------------------------------------
    float drive = 0.0f;
    if (!m_s.stalled && !neutral && !clutchIn && gear != 0) {
        // Engine pulls hardest low in the rev range, fades near the limiter.
        const float torqueFactor = std::clamp(1.15f - 0.55f * rpmNorm, 0.25f, 1.0f);
        const float gearForce = kBaseDrive * (gb.gearRatio[1] / ratio);
        float f = in.throttle * gearForce * torqueFactor;
        if (rpmNorm >= 1.0f) f = 0.0f;                 // at this gear's top speed
        drive = (gear < 0) ? -f : f;
    }
    if (gb.enabled && in.clutch > 0.0f && in.clutch <= gb.clutchBite && gear != 0) {
        // Partially engaged: slipping clutch delivers a fraction of the torque.
        drive *= 1.0f - (in.clutch / std::max(1e-3f, gb.clutchBite));
    }

    const float brakeF = in.brake * kBrakeMax * std::min(1.0f, grip);
    const float dragF  = 0.42f * m_s.vx * std::fabs(m_s.vx) + 180.0f * m_s.vx;

    float ax = (drive - dragF) / kMass;
    if (std::fabs(m_s.vx) > 0.2f)
        ax -= (m_s.vx > 0 ? 1.0f : -1.0f) * brakeF / kMass;
    else if (in.brake > 0.05f)
        m_s.vx = 0.0f;

    // ---- Lateral (bicycle model) ------------------------------------------
    const float u = std::max(1.2f, std::fabs(m_s.vx));   // avoid 1/0 at rest
    const float alphaF = std::atan2(m_s.vy + kA * m_s.yawRate, u) - delta;
    const float alphaR = std::atan2(m_s.vy - kB * m_s.yawRate, u);

    const float loadF = kMass * 9.81f * (kB / (kA + kB));
    const float loadR = kMass * 9.81f * (kA / (kA + kB));

    const float fyF = tyreLateral(alphaF, loadF, grip);
    const float fyR = tyreLateral(alphaR, loadR, grip);

    const float ay = (fyF + fyR) / kMass - m_s.vx * m_s.yawRate;
    const float yawAcc = (kA * fyF - kB * fyR) / kIzz;

    m_s.vx += ax * dt;
    m_s.vy += ay * dt;
    m_s.yawRate += yawAcc * dt;
    m_s.yawRate *= std::exp(-0.9f * dt);     // yaw damping

    m_s.heading += m_s.yawRate * dt;
    m_s.x += (m_s.vx * std::cos(m_s.heading) - m_s.vy * std::sin(m_s.heading)) * dt;
    m_s.y += (m_s.vx * std::sin(m_s.heading) + m_s.vy * std::cos(m_s.heading)) * dt;

    m_s.slipFront = alphaF;
    m_s.slipRear  = alphaR;
    m_s.loadFront = loadF;
    m_s.latForceFront = fyF;

    // ---- Force feedback sources -------------------------------------------
    // Self-aligning torque. Pneumatic trail shrinks as the tyre saturates, so
    // the force peaks just before the limit and then falls away -- the cue a
    // driver actually uses to feel understeer.
    const float trail = std::exp(-std::fabs(alphaF) / 0.14f);
    m_sat = std::clamp((fyF * trail) / 9000.0f, -1.0f, 1.0f);

    // Surface texture, scaled by speed.
    const float speed = std::sqrt(m_s.vx * m_s.vx + m_s.vy * m_s.vy);
    const float sp    = std::clamp(speed / 30.0f, 0.0f, 1.0f);
    float amp = 0.0f, freq = 0.0f;
    if (m_s.surface == SURF_KERB)       { amp = 0.55f; freq = 42.0f; }
    else if (m_s.surface == SURF_GRASS) { amp = 0.30f; freq = 17.0f; }

    if (amp > 0.0f) {
        m_textureAccum += dt * freq;
        const float wave = std::sin(m_textureAccum * 6.2831853f);
        m_texture = amp * sp * (0.75f * wave + 0.25f * frand());
    } else {
        m_texture *= std::exp(-12.0f * dt);
    }
}

float TestDrive::steeringForce(const FfbCfg& ffb) const {
    float f = m_sat * ffb.satScale + m_texture * ffb.detailGain;
    f *= ffb.masterGain;
    if (ffb.invertForce) f = -f;

    // Lift very small forces above the wheel's static friction so detail is
    // actually felt rather than swallowed by the gear train.
    if (ffb.minForce > 0.0f && std::fabs(f) > 1e-4f) {
        const float s = f < 0.0f ? -1.0f : 1.0f;
        f = s * (ffb.minForce + (1.0f - ffb.minForce) * std::fabs(f));
    }
    return std::clamp(f, -1.0f, 1.0f);
}
