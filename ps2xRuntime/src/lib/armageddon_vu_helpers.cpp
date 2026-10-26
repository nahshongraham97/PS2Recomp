// Armageddon VU0 macro helpers. Arithmetic implementation and reference
// fixtures are derived from OGmidway/mksm-recomp at 9c014184b54f8f4235db9327bf6a5d77566475c5.
// See tests/fixtures/LICENSE.GPL3.txt for the GPL-3.0 license terms.
#include "armageddon_vu_sub.h"
#include "armageddon_vu_clip.h"

void armageddonVuSub(R5900Context *ctx, uint32_t word)
{
    ps2VuSub(ctx, word);
}

void armageddonVuClip(R5900Context *ctx, uint32_t word)
{
    ps2VuClip(ctx, word);
}
