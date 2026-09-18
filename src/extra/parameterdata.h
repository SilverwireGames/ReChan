#pragma once

#include "core.h"
#include "gen/config.h"
#include <string>

class Database;

// WDB <-> typed JSON bridge used by the DebugUI exporter and mod loader.
// JSON represents real editable game data; it is not an asset manifest.
bool ExportWDBParameters(const u8* data, u32 size, const std::string& outputPath);

#if MOD_LOADER
bool ApplyWDBParameterOverrides(Database* database, const char* jsonPath);
bool VerifyWDBParameterRoundTrip(const u8* data, u32 size, const char* jsonPath);
#endif
