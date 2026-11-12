#pragma once

#include <windows.h>
#include <string>
#include <utility>
#include <vector>

constexpr DWORD kUnassigned = 0xFFFFFFFF;

// Calibration for one physical axis.
//
// Raw values arrive as 0..65535. Pedals rarely use their full travel and often
// rest at one end, so min/max are captured by actually moving them rather than
// assumed.
struct AxisCal {
    DWORD offset   = kUnassigned;
    LONG  rawMin   = 0;
    LONG  rawMax   = 65535;
    LONG  rawCenter= 32767;     // steering only
    bool  invert   = false;
    float deadzone = 0.02f;     // fraction removed at rest / centre
    float gamma    = 1.0f;      // 1 linear, <1 sharper off-centre, >1 softer

    bool assigned() const { return offset != kUnassigned; }

    // Steering: -1..+1 about the captured centre.
    float bipolar(LONG raw) const;
    // Pedals: 0..1 from rest to fully pressed.
    float unipolar(LONG raw) const;
};

// Simulated gearbox. The games we feed never see gears; this exists so the
// H-pattern shifter and clutch have real meaning, by shaping the throttle we
// output rather than by asking the game for anything.
struct GearboxCfg {
    bool  enabled      = true;
    int   gearCount    = 6;
    // Button index per position: [0] = reverse, [1..6] = gears 1-6. -1 = unset.
    int   gearButton[7]{-1, -1, -1, -1, -1, -1, -1};
    // Top speed fraction allowed in each gear, shaping throttle per gear.
    float gearRatio[7]{0.35f, 0.22f, 0.38f, 0.54f, 0.70f, 0.86f, 1.00f};
    bool  stallEnabled = true;
    float clutchBite   = 0.45f;  // clutch travel at which drive engages
    bool  neutralCutsThrottle = true;
};

struct FfbCfg {
    float masterGain       = 1.00f;
    float springGain       = 0.08f;   // light centring; the sim provides the rest
    float springSaturation = 1.00f;
    float damperGain       = 0.12f;
    float satScale         = 1.00f;   // self-aligning torque
    float detailGain       = 0.60f;   // surface texture / kerbs
    bool  invertForce      = false;
    float minForce         = 0.00f;   // lifts tiny forces over the dead zone

    // Measured force-response correction. Maps a requested force magnitude to
    // the magnitude actually sent, cancelling the wheel's own nonlinearity and
    // its dead zone around centre.
    bool        useLut  = false;
    std::string lutFile = "g29studio.lut";
    std::vector<std::pair<float, float>> lut;

    // Applies the LUT to a signed force, preserving direction. Returns the
    // input unchanged when no usable curve is loaded.
    float applyLut(float n) const;
};

// Output to the virtual Xbox pad -- how the wheel reaches other games.
struct BridgeCfg {
    bool  enabled      = false;
    float steerScale   = 1.00f;   // wheel travel mapped onto full stick deflection
    bool  shapeThrottle= true;    // let the simulated gearbox limit throttle
    float rumbleToWheel= 0.60f;   // game rumble converted into wheel vibration
    bool  mapButtons   = true;    // pass wheel buttons through as pad buttons
};

struct Profile {
    std::string name = "G29 default";

    AxisCal steering, throttle, brake, clutch;
    GearboxCfg gearbox;
    FfbCfg     ffb;
    BridgeCfg  bridge;

    int steeringRangeDeg = 900;

    bool save(const std::string& path, std::string& err) const;
    bool load(const std::string& path, std::string& err);
};

// Applies deadzone + gamma to an already-normalised value.
float shape(float v, float deadzone, float gamma);
