#ifndef RESTIR_PT_PREPASS_SORT_H
#define RESTIR_PT_PREPASS_SORT_H

// Spatial pre-pass work items
// ReSTIR PT Enhanced, Section 6.2.2: the pre-pass shifts one path per (pixel, pairing slot), and those shifts are wildly uneven.
// Measured on Cornell Box, 48.7% replay no prefix at all, 15.3% have no reconnection anchor and replay all the way to their endpoint, and a long tail runs up to eight prefix bounces.
// A warp executes its slowest lane, and with a 32-lane warp there is an ~89% chance of containing a lane with k>=5, so warps routinely perform seven traces to serve a median lane that needs two.
// This file was written for a version that sorted the work list by divergence class. The sort has since been removed (see ReSTIRPTPrepassClassify.hlsl for why); what remains here is the shared packing of a work item.
// The classify pass that writes an item and the traced pass that reads it must agree on this layout, so both call these helpers rather than re-deriving it.

#include "ReSTIR/PTParameters.h"

// One (pixel, slot) pair packed into a single word.
// Slot is two bits because RESTIR_PT_MAX_PAIRING_TEXTURES is 3, leaving 30 bits of pixel index, far more than any viewport this renderer supports.
uint PackPrepassWorkItem(uint pixelIndex, uint slot)
{
  return (pixelIndex << 2) | (slot & 3u);
}

void UnpackPrepassWorkItem(uint packed, out uint pixelIndex, out uint slot)
{
  pixelIndex = packed >> 2;
  slot       = packed & 3u;
}

#endif


