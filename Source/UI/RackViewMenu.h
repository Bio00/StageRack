#pragma once

#include "../Engine/Session.h"
#include "EditController.h"
#include "SendsPanel.h"

namespace stagerack
{

/** The VIEW menu (Pro Tools "Mix window view"): which sections every rack shows, and which
    sends. Saved with the session (not an undo step). Shared by the top bar's VIEW button and the
    menu bar's View menu. A rack can still have its own sections and sends (••• → Rack View, rackOwnViewMenu). */
inline juce::PopupMenu rackViewMenu (Session& session)
{
    const auto v = session.getRackView();
    auto& engine = session.getEngine();

    auto toggle = [&session] (bool Session::RackView::* field)
    {
        return [&session, field]
        {
            auto nv = session.getRackView();
            nv.*field = ! (nv.*field);
            session.setRackView (nv);
        };
    };

    juce::PopupMenu m;
    m.addSectionHeader ("Rack sections");
    m.addItem ("I/O Selectors", true, v.io, toggle (&Session::RackView::io));
    m.addItem ("Input (trim + meter)", true, v.input, toggle (&Session::RackView::input));
    m.addItem ("Inserts (plugins)", true, v.inserts, toggle (&Session::RackView::inserts));
    m.addItem ("Sends", engine.isRoutingEnabled(), v.sends, toggle (&Session::RackView::sends));
    m.addItem ("Info (core, load, latency)", true, v.info, toggle (&Session::RackView::info));

    if (engine.isRoutingEnabled())
    {
        auto setMask = [&session] (std::function<int (int)> change)
        {
            return [&session, change]
            {
                auto nv = session.getRackView();
                nv.sendsMask = change (nv.sendsMask) & 0xff;
                nv.sends = true;
                session.setRackView (nv);
            };
        };

        m.addSectionHeader ("Sends shown");
        m.addItem ("All", true, v.sends && v.sendsMask == 0xff, setMask ([] (int) { return 0xff; }));

        for (int k = 1; k <= Rack::maxBuses; ++k)
        {
            auto* bus = engine.findBus (k);
            if (bus == nullptr || bus->isFolder)
                continue;

            const auto letter = juce::String::charToString ((juce::juce_wchar) ('A' + k - 1));
            const auto bit = 1 << (k - 1);
            m.addItem ("Bus " + letter + (bus->name == "Bus " + letter ? juce::String() : "  " + bus->name),
                       true, v.sends && (v.sendsMask & bit) != 0, setMask ([bit] (int cur) { return cur ^ bit; }));
        }

        int overrides = 0;
        for (int i = 0; i < engine.getNumRacks(); ++i)
            overrides += engine.getRack (i).uiSendsShown >= 0 ? 1 : 0;

        m.addItem ("Racks with their own sends: " + juce::String (overrides) + "  (reset all)", overrides > 0, false, [&session]
        {
            auto& e = session.getEngine();
            for (int i = 0; i < e.getNumRacks(); ++i)
                if (e.getRack (i).uiSendsShown >= 0)
                    session.setRackSendsShown (e.getRack (i), -1);
        });
    }

    int ownSections = 0;
    for (int i = 0; i < engine.getNumRacks(); ++i)
        ownSections += engine.getRack (i).uiHideSections >= 0 ? 1 : 0;

    if (ownSections > 0)
        m.addItem ("Racks with their own sections: " + juce::String (ownSections) + "  (reset all)", true, false, [&session]
        {
            auto& e = session.getEngine();
            for (int i = 0; i < e.getNumRacks(); ++i)
                if (e.getRack (i).uiHideSections >= 0)
                    session.setRackHiddenSections (e.getRack (i), -1);
        });

    m.addSeparator();
    m.addItem ("Show everything", ! (v == Session::RackView()), false, [&session] { session.setRackView ({}); });
    return m;
}

/** One rack's own view (rack menu ••• → Rack View): its sections instead of the VIEW menu's,
    e.g. only the sends on an FX-return rack while every other rack shows everything. Alt = every
    selected rack. */
inline juce::PopupMenu rackOwnViewMenu (Session& session, EditController& edit, Rack& rack)
{
    const int id = rack.getId();
    const auto own = rack.uiHideSections;
    const auto hidden = Session::hiddenBits (session.viewFor (rack));

    // applied when chosen, to this rack (Alt: every selected rack too); the rack is found again by id
    auto apply = [&session, &edit, id] (std::function<int (int currentHidden)> change)
    {
        return [&session, &edit, id, change]
        {
            auto* r = session.getEngine().findRack (id);
            if (r == nullptr) return;
            auto all = edit.linkedRacks (*r, juce::ModifierKeys::getCurrentModifiersRealtime());
            all.add (r);
            for (auto* x : all)
                session.setRackHiddenSections (*x, change (Session::hiddenBits (session.viewFor (*x))));
        };
    };

    constexpr int io = 1, input = 2, inserts = 4, sends = 8, info = 16;
    const bool routing = session.getEngine().isRoutingEnabled();

    juce::PopupMenu m;
    m.addItem ("As the view (VIEW menu)", true, own < 0, apply ([] (int) { return -1; }));
    m.addItem ("Only sends", routing, own >= 0 && own == (io | input | inserts | info), apply ([] (int) { return io | input | inserts | info; }));
    m.addItem ("Only inserts", true, own >= 0 && own == (io | input | sends | info), apply ([] (int) { return io | input | sends | info; }));
    m.addItem ("Everything", true, own == 0, apply ([] (int) { return 0; }));
    m.addSectionHeader ("Sections of this rack");

    auto section = [&] (const juce::String& name, int bit, bool enabled = true)
    {
        m.addItem (name, enabled, (hidden & bit) == 0, apply ([bit] (int cur) { return cur ^ bit; }));
    };
    section ("I/O Selectors", io);
    section ("Input (trim + meter)", input);
    section ("Inserts (plugins)", inserts);
    section ("Sends", sends, routing);
    section ("Info (core, load, latency)", info);

    if (routing)
    {
        m.addSeparator();
        m.addSubMenu ("Sends shown", SendsPanel::sendsShownMenu (session, edit, rack));
    }

    return m;
}

} // namespace stagerack
