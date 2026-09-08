/*
 * ReSTIR PT Enhanced - Section 6.2.2, pass 2 of 2: publish the launch size.
 *
 * The work list length lives only on the GPU, so the traced pass is dispatched
 * indirectly and this writes its dimensions. A single thread, because there is one
 * number to copy.
 */

#include <nvshaders/slang_types.h>
#include "ShaderIo.h"
#include "ShaderIncludes/ReSTIR/PTGlobals.h.slang"

[shader("compute")]
[numthreads(1, 1, 1)]
void main()
{
  const uint total = ptPrepassCounterBuffer[RESTIR_PT_PREPASS_COUNT_OFFSET];

  // VkTraceRaysIndirectCommandKHR: width, height, depth. One-dimensional, because
  // the work list is a flat array and its index is the only coordinate the traced
  // pass wants. Zero height and depth when empty, so nothing is launched.
  ptPrepassCounterBuffer[RESTIR_PT_PREPASS_INDIRECT_OFFSET + 0] = total;
  ptPrepassCounterBuffer[RESTIR_PT_PREPASS_INDIRECT_OFFSET + 1] = total > 0 ? 1u : 0u;
  ptPrepassCounterBuffer[RESTIR_PT_PREPASS_INDIRECT_OFFSET + 2] = total > 0 ? 1u : 0u;
}
