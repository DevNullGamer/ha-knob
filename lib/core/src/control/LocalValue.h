#pragma once

// A control's local (optimistic) value while it holds (plan-step7.md Q7-1): produced by
// control::ControlEngine::local(), shown by presenter::present() (PresentContext::local).
// Separate header so the presenter does not depend on the engine.
namespace core {
namespace control {

struct LocalValue {
    double value;      // display units, already stepped
    bool unconfirmed;  // true while holding (always, as local() is empty otherwise)
};

}  // namespace control
}  // namespace core
