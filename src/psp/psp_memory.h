#pragma once

#include <cstdlib>

// Whether this run has the large PSP-2000+ user heap (MEMSIZE=1 EBOOT, about
// 45 MB) rather than the ~17.5 MB a PSPLink PRX gets. Data that is optional
// but avoids Memory Stick reads during gameplay (resident sound banks, voice
// clip cache) is only kept in the large heap. Probed once with an allocation
// only that heap can satisfy; the block is released immediately.
inline bool PspHasLargeHeap() {
    static int cached = -1;
    if (cached < 0) {
        void* probe = std::malloc(24u * 1024u * 1024u);
        cached = probe != nullptr ? 1 : 0;
        std::free(probe);
    }
    return cached != 0;
}
