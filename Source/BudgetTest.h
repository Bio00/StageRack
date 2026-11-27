#pragma once

#include "Engine/BudgetAdvisor.h"

namespace stagerack
{

/** BudgetAdvisor checks (part of --selftest): times given explicitly, no device. */
inline void runBudgetSelfTest (const std::function<void (bool, const juce::String&)>& check)
{
    const juce::Array<int> sizes { 16, 32, 64, 128, 256 };
    const double sr = 48000.0;

    // FIERA at 16: De-Feedback hop + soothe3 in series peak at 0.35 ms of 0.333 ms
    {
        BudgetAdvisor b;
        b.update (0.0, 16, sr, sizes, 0.15);
        b.event (1.0, 0.36, 0.35f, 0.333f, false, 2);
        b.event (2.0, 0.30, 0.29f, 0.333f, false, 2);
        b.update (2.1, 16, sr, sizes, 0.15);
        check (! b.isActive(), "budget: two dropouts in 10 s are not enough");

        b.event (4.0, 0.31, 0.30f, 0.333f, false, 2);
        b.update (4.1, 16, sr, sizes, 0.15);
        check (b.isActive() && b.getWorstRack() == 2, "budget: three heavy dropouts in 10 s -> warning, heaviest rack known");
        check (b.getRecommended() == 32, "budget: 0.36 ms of hop at 16 samples -> use 32 (got " + juce::String (b.getRecommended()) + ")");

        b.update (60.0, 16, sr, sizes, 0.15);
        check (b.isActive(), "budget: the warning stays for a minute");
        b.update (64.5, 16, sr, sizes, 0.15);
        check (! b.isActive(), "budget: a clean minute clears it");
    }

    // a stall of the device while we were nearly idle: a bigger buffer would not help
    {
        BudgetAdvisor b;
        b.update (0.0, 32, sr, sizes, 0.1);
        for (int k = 0; k < 5; ++k)
            b.event (1.0 + k, 0.05f, 0.029f, 0.667f, false, 1);
        b.update (6.0, 32, sr, sizes, 0.1);
        check (! b.isActive(), "budget: dropouts with light processing (device stall) -> no advice");
    }

    // silent blocks (racks being edited) never count
    {
        BudgetAdvisor b;
        b.update (0.0, 16, sr, sizes, 0.1);
        for (int k = 0; k < 5; ++k)
            b.event (1.0 + k, 0.4f, 0.4f, 0.333f, true, 1);
        b.update (6.0, 16, sr, sizes, 0.1);
        check (! b.isActive(), "budget: silent blocks are not a buffer problem");
    }

    // a continuous heavy chain grows with the buffer: needs a much bigger one
    {
        BudgetAdvisor b;
        b.update (0.0, 32, sr, sizes, 0.9);
        for (int k = 0; k < 3; ++k)
            b.event (1.0 + k, 0.7f, 0.7f, 0.667f, false, 1);
        b.update (4.0, 32, sr, sizes, 0.9);
        const auto r = b.getRecommended();
        check (r == 0 || r == 256, "budget: a chain that scales with the buffer never fits -> the largest size (got " + juce::String (r) + ")");

        b.update (5.0, 64, sr, sizes, 0.9);
        check (! b.isActive(), "budget: a new buffer is judged afresh");
    }
}

} // namespace stagerack
