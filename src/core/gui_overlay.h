#pragma once

// GUI XML overlay — serve modified OptionPanel/Root.xml and Views/Skills.xml
// from memory instead of writing them to disk.
//
// AOReloaded used to append its options tab (and the Favorites group) to the
// game's GUI XML files on disk. Those edits survived deleting version.dll,
// so users who uninstalled by removing the DLL kept the custom GUI. This
// module intercepts the process's file opens instead: when the client opens
// one of the registered GUI files, the original bytes are read, the patcher
// rewrites them, and the caller gets a transient (delete-on-close) copy of
// the result. Nothing in the game install is modified, so uninstalling is
// simply deleting version.dll again.
//
// The first run of a build with this module also strips any blocks left on
// disk by older builds (see the Clean* functions in settings.cpp and
// skill_favorites.cpp), so existing users are repaired too.

#include <string>

namespace aor::overlay {

// Rewrites `in` into `out`. Returns true to serve `out`, false to leave the
// file untouched. Called on the game thread, inside a file-open call, so it
// must be fast and must not itself open the file being patched.
using Patcher = bool (*)(const std::string& in, std::string& out);

// Register a patcher for the file whose path ends in `relPath` (either
// slash direction, case-insensitive), e.g. "OptionPanel/Root.xml". Up to 8
// rules. Call before Init().
void RegisterPatcher(const char* relPath, Patcher fn);

// Install the CreateFileW/CreateFileA hooks. Call as early as possible, and
// again once the game's DLLs are all loaded (idempotent).
void Init();

}  // namespace aor::overlay
