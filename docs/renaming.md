# Naming and compatibility — 2026-09-07

Infinity is now **unendlich**; Infinity Engine is **unendlich engine**.
The CMake project names are `unendlich` and `unendlich-engine`. Both still
live in this repository, in the unchanged `game/` and `engine/` directories.

Build target names remain `game_app`, `game_cli`, `game_tests`, `engine_tests`,
`engine_core`, `engine_world`, `engine_render`, and `cityblock`; CMake aliases
`engine::*` and `game::*` remain unchanged. Executables are now
`build/game/app/unendlich` and `build/game/cli/unendlich-cli`. Update launch
scripts to those names. No old executable aliases are installed; an old binary
left in a reused build directory is stale. Prefer a fresh build directory.
The macOS distribution is `unendlich.app` / `unendlich-<version>-<arch>.dmg`.

Use `UNENDLICH_BUILD_APP`, `UNENDLICH_BUILD_DEMOS`, and
`UNENDLICH_ENGINE_TESTS`. Their `INFINITY_` spellings remain accepted when the
corresponding new option is absent. Once the new option exists in a CMake cache,
change that option explicitly; the old value no longer overrides it. This also
works when configuring `engine/` standalone.

Asset lookup order is `--assets`, `UNENDLICH_ASSETS`, legacy `INFINITY_ASSETS`,
assets beside the executable, then the source assets directory. Invalid manifest
paths are skipped. CI accepts `UNENDLICH_SKIP_APP_SMOKE` and the former
`INFINITY_SKIP_APP_SMOKE`. Compatibility inputs have no removal date yet.

The C++ namespace `inf` is retained as a source and binary compatibility
contract, not a display name. `inf::core::tree::UnendlichTree` is an alias of
the existing `InfinityTree` type: both spellings designate the same type and
retain its symbols. New game call sites and design guidance use `UnendlichTree`.
The Metal bridge symbol is now `unendlichMetalLayerForCocoaWindow`; rebuild
engine and demo together. No installed library package or external bridge ABI
is declared in this repository.

Persistence is intentionally unchanged: macOS uses
`~/Library/Application Support/Infinity/`, files retain the `infinity-` prefix,
and the bundle ID remains `com.psiori.infinity`. Do not rename or duplicate save
files for this branding change. Explicit `--diff` paths still work. Registry
names, numeric IDs, subsystem/version keys, seeds, serialization and golden
fixtures are unchanged. Mathematical uses of “infinity” in rendering are also
unchanged. The design document path `design/infinity-tree.md` remains stable in
the research workspace.
