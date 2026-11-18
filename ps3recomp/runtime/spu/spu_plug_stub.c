/*
 * ps3recomp - optional WWS plug-in lookup
 *
 * A port that lifts the title's WWS SPU plug-in blobs (tools/plug_lift.py,
 * infamous-recomp/spu/wwsplug) supplies a strong spu_plug_lookup(). A tree
 * without them -- the toolkit smoke build, a port with PORT_ENABLE_SPU=OFF --
 * still links: this weak fallback answers "not a plug-in", so the interpreter
 * handles the code as it did before the plug-in lift existed.
 *
 * POSIX only: Windows has no weak data/function definition in the same place,
 * and every Windows port in this tree ships the generated lookup.
 */
#include "spu_context.h"

#ifndef _WIN32

typedef void (*spu_plug_fn)(spu_context*);

__attribute__((weak)) spu_plug_fn spu_plug_lookup(spu_context* ctx, uint32_t pc)
{
    (void)ctx;
    (void)pc;
    return 0;
}

#endif /* !_WIN32 */
