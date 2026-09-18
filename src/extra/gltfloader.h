#pragma once
#include "core.h"
#include "gen/config.h"

#if MOD_LOADER

struct OriginalSTree;
struct OriginalGeo;
struct OriginalETree;

// Loads GLB (glTF binary) model files and produces the same Original* types
// that the game's P3D stream parser normally creates from .RR files.
// Uses cgltf (vendor/cgltf/cgltf.h) for parsing.
class GLTFLoader {
public:
    // Load a skeletal mesh (STree) from a GLB file.
    // Returns nullptr on failure. Caller owns the result.
    static OriginalSTree* LoadSTree(const char* path);

    // Load a static geometry mesh from a GLB file.
    // Returns nullptr on failure. Caller owns the result.
    static OriginalGeo* LoadGeo(const char* path);

    // Load an export-tree hierarchical mesh from a GLB file.
    // Returns nullptr on failure. Caller owns the result.
    static OriginalETree* LoadETree(const char* path);
};

#endif // MOD_LOADER