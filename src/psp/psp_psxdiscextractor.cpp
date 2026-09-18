#include "extra/psxdiscextractor.h"
#include "p3d/fileio.h"

PsxDiscExtractor* g_psxDiscExtractor = nullptr;

void PsxDiscExtractor::Init() {
    m_state = AreAssetsPresent() ? State::Done : State::ScanNoneFound;
}

void PsxDiscExtractor::Shutdown() {}
void PsxDiscExtractor::ScanAsync() {}
void PsxDiscExtractor::StartExtractAsync() {}

float PsxDiscExtractor::GetProgress() const {
    return m_state == State::Done ? 1.0f : 0.0f;
}

bool PsxDiscExtractor::AreAssetsPresent() {
    return p3d::io::DirExists("xc") && p3d::io::DirExists("fe")
        && p3d::io::DirExists("tim") && p3d::io::FileExists("license.tim");
}
