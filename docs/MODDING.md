# ReChan drag-and-drop mods

Place each mod in its own folder under `~mods`. No manifest is required.
The mod folder name is the mod name. Mods load alphabetically unless
`~mods/mods.ini` specifies a load order; later mods win conflicts.

When the game is built with `MOD_LOADER`, open **Options > Mods** to browse
every mod folder and enable or disable it. The list scrolls with menu controls
or the mouse wheel. Changes are saved to the `[blacklist]` section of
`~mods/mods.ini` and the override index reloads immediately; reload the current
level (or restart the game) to replace assets that were already in memory.

Asset identity comes from its filename. Recognized `levels/<level>` and
`characters/<character>` folders provide an optional scope. A scoped asset is
preferred over a global asset with the same name.

```text
~mods/MyMod/
  textures/name.png
  sounds/name.wav
  sounds/rs0000/sample00.wav
  sounds/dialog/c01_d010_v00.wav
  geometry/name.glb
  levels/lev01/
    geometry/barrel.glb
    textures/wall.png
    parameters_petal00.json
  characters/jackie/
    jackie.glb
    textures/face.png
```

- PNG replaces a texture with the same asset name.
- WAV replaces a sound with the same asset name.
- GLB replaces a model or geometry resource with the same asset name.
- JSON is reserved for typed game and level parameters.

Invalid overrides are ignored and the original game asset remains active.

Fresh DebugUI dumps embed baseline metadata directly inside each GLB and PNG.
An untouched dumped file keeps the native engine asset active, preserving
engine-only behavior, materials, palettes, and lighting exactly. Editing and
saving the file changes/removes that baseline match and activates the override.
This metadata is contained in the asset itself; there is no sidecar or manifest.

Dialogue audio is exported from `RSDIALOG.DLG` as
`sounds/dialog/cCC_dDDD_vVV.wav`: character index, dialog ID, and randomized
variant index. Keep that filename to replace exactly that spoken variant.

## Subtitles

Movie subtitle overrides are loaded from `.srt` files in a mod. The runtime
matches the subtitle by the movie filename stem, so a file named like
`intro.srt` will override the movie `intro.str`, `intro.avi`, `intro.mkv`, or similar aslong as the base name matches.

Put the file anywhere under the mod folder; it does not need a specific
folder layout. The important part is the filename itself.

```text
~mods/MyMod/
  movies/
    intro.srt
```

Examples:
- `credits.srt` overrides `credits.*`
- `factory.srt` overrides `factory.*`
- `making.srt` overrides `making.*`
- `prolog.srt` overrides `prolog.*`
- `victory.srt` overrides `victory.*`

Use a standard SRT file with timecodes and UTF-8 text. A BOM is accepted as
well; the loader strips a UTF-8 BOM automatically.

```srt
1
00:00:10,000 --> 00:00:14,000
Hello there.

2
00:00:15,000 --> 00:00:18,500
This is a subtitle override.
```

If the movie file has a different extension but the same base name, the same
`.srt` file still applies. In other words: same stem, same subtitle.

## Level parameters

The DebugUI exporter writes one `parameters_petalNN.json` file for each WDB
database in a level. These files use the schema
`rechan.level-parameters.v1`. Objects are matched by `kind`, `name`, and their
stable `ordinal`, then
validated fields such as positions, bounds, paths, mesh filenames, and typed
attributes are applied before blocks and gameplay objects are constructed.
Generated files include a `sourceFingerprint`. The loader refuses to apply a
file to a different level or petal database, and unchanged values are true
no-ops.

For a hand-written mod, `parameters.json` is also accepted as a fallback for
every petal. Unknown objects, fields, or attributes are ignored. A malformed
file leaves the original database unchanged.

Visual GLB geometry overrides do not replace the game's collision geometry.
Collision-related paths, volumes, bounds, and other database objects should be
edited through the typed parameter JSON when available.
