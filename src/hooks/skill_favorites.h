#pragma once

// Skills window (SkillWindow, GUI.dll) additions:
//
// Favorites — a "Favorites" group above "Abilities". Ctrl+Alt+click a skill
// in any group to add it to (or remove it from) the favorites; favorite
// skills show "* " before their name. Points are raised and lowered in the
// Favorites group like in any other group.
//
//   The window builds its groups from a table (name, list of stats) filled
//   by one function, then makes one StatRow per stat and per view (the
//   compact "minimized" list and the full two-column page) and drops them
//   into the views named "<group>_view" and "<group>_group" of Skills.xml.
//   We append a 12th group "favorites" that lists every skill (favorites
//   first) and add its two views to Skills.xml; its rows for skills that
//   aren't favorites are hidden, so toggling a favorite just shows or hides
//   a row — no window rebuild.
//
//   Each StatRow keeps its own pending points (+0x1b0), so a skill that is
//   in Favorites and in its own group has two rows that must agree. Every
//   change goes through StatRow::ChangeDelta; after it we copy the new
//   pending points (and the IP cost already charged for them) to the other
//   rows of the same skill and view and let them redraw with a zero change,
//   which is what the stock code does when it switches between the compact
//   and full views. The IP counter is only charged once, by the row that
//   was clicked. "Suggested IP distribution" walks the group table and
//   would count a skill twice, so the favorites group is hidden from it.
//
// Shift+click — Shift+click on a skill's + or - changes it by 5 instead of 1
//   (fewer if the skill cap, the points pending or the IP run out). Stock
//   Ctrl/Alt+click still goes all the way (cap / zero); stock did that for
//   Shift too.
//
// Favorites are per character, saved in AOReloadedSkills.ini next to the
// exe. Toggled by the AOR_SkillFavs and AOR_SkillShift DValues (default on).

namespace aor {

// Register the in-memory Views/Skills.xml overlay (serves the favorites
// views without writing the file) and, once, strip any fragments older
// builds wrote to disk. Call before the skills window is first opened, e.g.
// next to RegisterRootXmlOverlay().
void RegisterSkillsXmlOverlay();
void CleanSkillsXmlOnDisk();

// Install the hooks. GUI.dll is a static import of the exe.
// Returns false (and installs nothing) if the client doesn't match.
bool InitSkillFavorites();

}  // namespace aor
