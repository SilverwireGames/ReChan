#pragma once

#include "pddi/pddishadersrc.h"

// Installs rechan's GLSL as the source provider for whichever pddi backend is
// active. Must be called before the render context is created, since the
// backend compiles its internal programs during construction.
void RechanInstallShaderSources();
