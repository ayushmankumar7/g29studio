#pragma once

#include "profile.h"

enum Surface { SURF_ASPHALT = 0, SURF_KERB = 1, SURF_GRASS = 2 };

struct CarInput {
    float steer    = 0.0f;   // -1..+1
    float throttle = 0.0f;   // 0..1
    float brake    = 0.0f;   // 0..1
    float clutch   = 0.0f;   // 0..1  (1 = fully pressed / disengaged)
    int   gear     = 0;      // -1 reverse, 0 neutral, 1..6
};

struct CarState {
    float x = 0.0f, y = -140.0f;   // world position, metres
    float heading = 0.0f;          // radians
    float vx = 0.0f, vy = 0.0f;    // body-frame velocity, m/s
    float yawRate = 0.0f;

    float engineRpm = 800.0f;
    bool  stalled   = false;

    float slipFront = 0.0f, slipRear = 0.0f;  // radians
    float loadFront = 0.0f;                   // N
    float latForceFront = 0.0f;
    int   surface   = SURF_ASPHALT;
    float grip      = 1.0f;

    float speedKph() const;
};

// A deliberately small but honest vehicle model: a bicycle model with a
// saturating tyre curve. It exists so force feedback has real physics behind
// it -- slip angle, load and surface -- which is the one thing a controller
// bridge can never obtain from a game it does not modify.
class TestDrive {
public:
    void reset();
    void step(float dt, const CarInput& in, const GearboxCfg& gb);

    const CarState& state() const { return m_s; }

    // Steering force to send to the wheel, -1..+1, before user gains.
    // Combines self-aligning torque with surface texture.
    float steeringForce(const FfbCfg& ffb) const;

    // Track geometry: an oval centreline. Returns signed distance from it.
    static float trackOffset(float x, float y);
    static int   surfaceAt(float x, float y, float* gripOut);

    static constexpr float kTrackRadiusX = 170.0f;
    static constexpr float kTrackRadiusY = 110.0f;
    static constexpr float kHalfWidth    = 9.0f;
    static constexpr float kKerbWidth    = 2.0f;

private:
    CarState m_s;
    float    m_textureAccum = 0.0f;   // phase for kerb / gravel vibration
    float    m_texture      = 0.0f;   // current texture force, -1..1
    float    m_sat          = 0.0f;   // current self-aligning torque, -1..1
};
