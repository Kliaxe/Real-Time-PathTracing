#ifndef RESTIR_PT_PREPASS_SORT_H
#define RESTIR_PT_PREPASS_SORT_H

/*
 * ReSTIR PT Enhanced - sorting the spatial pre-pass by divergence class (6.2.2).
 *
 * The pre-pass shifts one path per (pixel, pairing slot). Those shifts are wildly
 * uneven: measured on Cornell Box, 48.7% replay no prefix at all, 15.3% have no
 * reconnection anchor and replay all the way to their endpoint, and a long tail
 * runs up to eight prefix bounces. A warp executes its slowest lane, and with a
 * 32-lane warp there is an ~89% chance of containing a lane with k>=5 - so warps
 * routinely perform seven traces to serve a median lane that needs two.
 *
 * The fix is not to discard work. Survivor density here is high (pre-pass cost
 * scales linearly with slot count), so there is little to compact away. It is to
 * REORDER: group pairs of similar cost so a warp of cheap lanes finishes cheaply.
 *
 * The shared classification below is what keeps the three passes agreeing. The
 * counting pass, the scatter pass and the traced pass must all decide "does this
 * pair need work, and which bucket" identically, or entries are lost or double
 * counted - so they all call this, and none of them re-derive it.
 */

#include "ReSTIR/PTParameters.h"
// ReSTIRPTReservoir, whose pathFlags the classification reads.
#include "ReSTIR/PTReservoir.hlsli"

// One (pixel, slot) pair packed into a single word. Slot is two bits because
// RESTIR_PT_MAX_PAIRING_TEXTURES is 3, leaving 30 bits of pixel index - far more
// than any viewport this renderer supports.
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
