/*
 * SPDX-License-Identifier: MIT
 *
 * Workarounds for D3DMetal defects that only its private object layout
 * can reach (npt_d3dmetal_quirks.c).
 */

#ifndef NPT_D3DMETAL_QUIRKS_H
#define NPT_D3DMETAL_QUIRKS_H

#include "npt_common.h"

/* Object-registration hook: runs for every new guest id, QueryInterface
 * aliases included, so the same host object can come through more than
 * once.  Zeroes the garbage vertex-slot count of a no-op D3DMetal
 * graphics pipeline state before any draw can walk it.  Silent no-op for
 * every other object. */
void
npt_d3dmetal_on_register_object(void *obj, npt_object_type type);

#endif /* NPT_D3DMETAL_QUIRKS_H */
