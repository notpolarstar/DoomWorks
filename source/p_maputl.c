/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 *      Movement/collision utility functions,
 *      as used by function in p_map.c.
 *      BLOCKMAP Iterator functions,
 *      and some PIT_* functions to use for iteration.
 *
 *-----------------------------------------------------------------------------*/

#include "doomstat.h"
#include "m_bbox.h"
#include "r_main.h"
#include "p_maputl.h"
#include "p_map.h"
#include "p_setup.h"

#include "global_data.h"

#if defined(NUMWORKS) && PLATFORM_DEVICE
#define GBADOOM_COMPACT_BLOCKLINKS 1
#else
#define GBADOOM_COMPACT_BLOCKLINKS 0
#endif

static int P_BlockIndexForThing(const mobj_t* thing)
{
    int blockx = (thing->x - _g->bmaporgx) >> MAPBLOCKSHIFT;
    int blocky = (thing->y - _g->bmaporgy) >> MAPBLOCKSHIFT;

    if (blockx < 0 || blockx >= _g->bmapwidth || blocky < 0 || blocky >= _g->bmapheight)
        return -1;

    return blocky * _g->bmapwidth + blockx;
}

static unsigned int P_ReadU16LE_Byte(const byte* p)
{
    return ((unsigned int)p[0]) | ((unsigned int)p[1] << 8);
}

static unsigned int P_BitCount8(unsigned int v)
{
    v &= 0xFFu;
    v = v - ((v >> 1) & 0x55u);
    v = (v & 0x33u) + ((v >> 2) & 0x33u);
    return (v + (v >> 4)) & 0x0Fu;
}

static boolean P_VisitBlockLineByIndex(int lineno, boolean func(const line_t*), int vcount)
{
    linedata_t *lt;
    const line_t *ld;

    if ((unsigned int)lineno >= (unsigned int)_g->numlines)
        return true;

    lt = &_g->linedata[lineno];
    if (lt->validcount == vcount)
        return true;

    lt->validcount = vcount;
    ld = &_g->lines[lineno];
    return func(ld);
}

static boolean P_BlockLinesIteratorCompact(int x, int y, boolean func(const line_t*))
{
    const byte *row;
    const byte *bitmap;
    const byte *desc_ptr;
    unsigned int desc_count;
    unsigned int bitmap_bytes = _g->blockmap_compact_bitmap_bytes;
    unsigned int byte_index;
    unsigned int bit_mask;
    unsigned int desc_index = 0;
    unsigned int b;
    unsigned int descriptor;
    int vcount = _g->validcount;

    row = _g->blockmap_compact_data + P_ReadU16LE_Byte(_g->blockmap_compact_rowofs + (y * 2));
    if (row + 2 > _g->blockmap_compact_end)
        I_Error("P_BlockLinesIteratorCompact: truncated row header");

    desc_count = P_ReadU16LE_Byte(row);
    bitmap = row + 2;
    desc_ptr = bitmap + bitmap_bytes;
    if (desc_ptr + (desc_count * 2u) > _g->blockmap_compact_lists)
        I_Error("P_BlockLinesIteratorCompact: row descriptor overflow");

    byte_index = (unsigned int)x >> 3;
    bit_mask = 1u << ((unsigned int)x & 7u);
    if ((bitmap[byte_index] & bit_mask) == 0)
        return true;

    for (b = 0; b < byte_index; b++)
        desc_index += P_BitCount8(bitmap[b]);
    desc_index += P_BitCount8(bitmap[byte_index] & (bit_mask - 1u));

    if (desc_index >= desc_count)
        I_Error("P_BlockLinesIteratorCompact: invalid descriptor index %u/%u", desc_index, desc_count);

    descriptor = P_ReadU16LE_Byte(desc_ptr + (desc_index * 2u));
    if (descriptor & 0x8000u)
    {
        int lineno = (int)(descriptor & 0x7FFFu);
        return P_VisitBlockLineByIndex(lineno, func, vcount);
    }
    else
    {
        const byte *list = _g->blockmap_compact_lists + ((unsigned int)descriptor << 1);
        unsigned int count;
        unsigned int i;

        if (list + 2 > _g->blockmap_compact_end)
            I_Error("P_BlockLinesIteratorCompact: list offset overflow");

        count = P_ReadU16LE_Byte(list);
        list += 2;
        if (list + (count * 2u) > _g->blockmap_compact_end)
            I_Error("P_BlockLinesIteratorCompact: truncated list");

        for (i = 0; i < count; i++)
        {
            int lineno = (int)P_ReadU16LE_Byte(list + (i * 2u));
            if (!P_VisitBlockLineByIndex(lineno, func, vcount))
                return false;
        }
    }

    return true;
}

//
// P_AproxDistance
// Gives an estimation of distance (not exact)
//

fixed_t CONSTFUNC P_AproxDistance(fixed_t dx, fixed_t dy)
{
  dx = D_abs(dx);
  dy = D_abs(dy);
  if (dx < dy)
    return dx+dy-(dx>>1);
  return dx+dy-(dy>>1);
}

//
// P_PointOnLineSide
// Returns 0 or 1
//
// killough 5/3/98: reformatted, cleaned up

int PUREFUNC P_PointOnLineSide(fixed_t x, fixed_t y, const line_t *line)
{
  return
    !line->dx ? x <= LN_V1(line)->x ? line->dy > 0 : line->dy < 0 :
    !line->dy ? y <= LN_V1(line)->y ? line->dx < 0 : line->dx > 0 :
    FixedMul(y-LN_V1(line)->y, line->dx>>FRACBITS) >=
    FixedMul(line->dy>>FRACBITS, x-LN_V1(line)->x);
}

//
// P_BoxOnLineSide
// Considers the line to be infinite
// Returns side 0 or 1, -1 if box crosses the line.
//
// killough 5/3/98: reformatted, cleaned up

int PUREFUNC P_BoxOnLineSide(const fixed_t *tmbox, const line_t *ld)
{
    int p;
    switch (ld->slopetype)
    {

    default: // shut up compiler warnings -- killough
    case ST_HORIZONTAL:
        return
                (tmbox[BOXBOTTOM] > LN_V1(ld)->y) == (p = tmbox[BOXTOP] > LN_V1(ld)->y) ?
                    p ^ (ld->dx < 0) : -1;
    case ST_VERTICAL:
        return
                (tmbox[BOXLEFT] < LN_V1(ld)->x) == (p = tmbox[BOXRIGHT] < LN_V1(ld)->x) ?
                    p ^ (ld->dy < 0) : -1;
    case ST_POSITIVE:
        return
                P_PointOnLineSide(tmbox[BOXRIGHT], tmbox[BOXBOTTOM], ld) ==
                (p = P_PointOnLineSide(tmbox[BOXLEFT], tmbox[BOXTOP], ld)) ? p : -1;
    case ST_NEGATIVE:
        return
                (P_PointOnLineSide(tmbox[BOXLEFT], tmbox[BOXBOTTOM], ld)) ==
                (p = P_PointOnLineSide(tmbox[BOXRIGHT], tmbox[BOXTOP], ld)) ? p : -1;
    }
}

//
// P_PointOnDivlineSide
// Returns 0 or 1.
//
// killough 5/3/98: reformatted, cleaned up

static int PUREFUNC P_PointOnDivlineSide(fixed_t x, fixed_t y, const divline_t *line)
{
  return
    !line->dx ? x <= line->x ? line->dy > 0 : line->dy < 0 :
    !line->dy ? y <= line->y ? line->dx < 0 : line->dx > 0 :
    (line->dy^line->dx^(x -= line->x)^(y -= line->y)) < 0 ? (line->dy^x) < 0 :
    FixedMul(y>>8, line->dx>>8) >= FixedMul(line->dy>>8, x>>8);
}

//
// P_MakeDivline
//

static void P_MakeDivline(const line_t *li, divline_t *dl)
{
  dl->x = LN_V1(li)->x;
  dl->y = LN_V1(li)->y;
  dl->dx = li->dx;
  dl->dy = li->dy;
}

//
// P_InterceptVector
// Returns the fractional intercept point
// along the first divline.
// This is only called by the addthings
// and addlines traversers.
//

/* cph - this is killough's 4/19/98 version of P_InterceptVector and
 *  P_InterceptVector2 (which were interchangeable). We still use this
 *  in compatibility mode. */
fixed_t PUREFUNC P_InterceptVector2(const divline_t *v2, const divline_t *v1)
{
  fixed_t den;
  return (den = FixedMul(v1->dy>>8, v2->dx) - FixedMul(v1->dx>>8, v2->dy)) ?
    FixedDiv(FixedMul((v1->x - v2->x)>>8, v1->dy) +
             FixedMul((v2->y - v1->y)>>8, v1->dx), den) : 0;
}

//
// P_LineOpening
// Sets opentop and openbottom to the window
// through a two sided line.
// OPTIMIZE: keep this precalculated
//


void P_LineOpening(const line_t *linedef)
{
    if (linedef->sidenum[1] == NO_INDEX)      // single sided line
    {
        _g->openrange = 0;
        return;
    }

    _g->openfrontsector = LN_FRONTSECTOR(linedef);
    _g->openbacksector = LN_BACKSECTOR(linedef);

    if (_g->openfrontsector->ceilingheight < _g->openbacksector->ceilingheight)
        _g->opentop = _g->openfrontsector->ceilingheight;
    else
        _g->opentop = _g->openbacksector->ceilingheight;

    if (_g->openfrontsector->floorheight > _g->openbacksector->floorheight)
    {
        _g->openbottom = _g->openfrontsector->floorheight;
        _g->lowfloor = _g->openbacksector->floorheight;
    }
    else
    {
        _g->openbottom = _g->openbacksector->floorheight;
        _g->lowfloor = _g->openfrontsector->floorheight;
    }
    _g->openrange = _g->opentop - _g->openbottom;
}

//
// THING POSITION SETTING
//

//
// P_UnsetThingPosition
// Unlinks a thing from block map and sectors.
// On each position change, BLOCKMAP and other
// lookups maintaining lists ot things inside
// these structures need to be updated.
//

void P_UnsetThingPosition (mobj_t *thing)
{
  if (!(thing->flags & MF_NOSECTOR))
    {
      /* invisible things don't need to be in sector list
       * unlink from subsector
       *
       * killough 8/11/98: simpler scheme using pointers-to-pointers for prev
       * pointers, allows head node pointers to be treated like everything else
       */

      mobj_t **sprev = thing->sprev;
      mobj_t  *snext = thing->snext;
      if ((*sprev = snext))  // unlink from sector list
        snext->sprev = sprev;

        // phares 3/14/98
        //
        // Save the sector list pointed to by touching_sectorlist.
        // In P_SetThingPosition, we'll keep any nodes that represent
        // sectors the Thing still touches. We'll add new ones then, and
        // delete any nodes for sectors the Thing has vacated. Then we'll
        // put it back into touching_sectorlist. It's done this way to
        // avoid a lot of deleting/creating for nodes, when most of the
        // time you just get back what you deleted anyway.
        //
        // If this Thing is being removed entirely, then the calling
        // routine will clear out the nodes in sector_list.

      _g->sector_list = thing->touching_sectorlist;
      thing->touching_sectorlist = NULL; //to be restored by P_SetThingPosition
    }

  if (!(thing->flags & MF_NOBLOCKMAP))
    {
      /* inert things don't need to be in blockmap
       *
       * killough 8/11/98: simpler scheme using pointers-to-pointers for prev
       * pointers, allows head node pointers to be treated like everything else
       *
       * Also more robust, since it doesn't depend on current position for
       * unlinking. Old method required computing head node based on position
       * at time of unlinking, assuming it was the same position as during
       * linking.
       */

      mobj_t *bnext, **bprev = thing->bprev;
      if (bprev && (*bprev = bnext = thing->bnext))  // unlink from block map
        bnext->bprev = bprev;
    }
}

//
// P_SetThingPosition
// Links a thing into both a block and a subsector
// based on it's x y.
// Sets thing->subsector properly
//
// killough 5/3/98: reformatted, cleaned up

void P_SetThingPosition(mobj_t *thing)
{                                                      // link into subsector
  subsector_t *ss = thing->subsector = R_PointInSubsector(thing->x, thing->y);
  if (!(thing->flags & MF_NOSECTOR))
    {
      // invisible things don't go into the sector links

      // killough 8/11/98: simpler scheme using pointer-to-pointer prev
      // pointers, allows head nodes to be treated like everything else

      mobj_t **link = &ss->sector->thinglist;
      mobj_t *snext = *link;
      if ((thing->snext = snext))
        snext->sprev = &thing->snext;
      thing->sprev = link;
      *link = thing;

      // phares 3/16/98
      //
      // If sector_list isn't NULL, it has a collection of sector
      // nodes that were just removed from this Thing.

      // Collect the sectors the object will live in by looking at
      // the existing sector_list and adding new nodes and deleting
      // obsolete ones.

      // When a node is deleted, its sector links (the links starting
      // at sector_t->touching_thinglist) are broken. When a node is
      // added, new sector links are created.

      P_CreateSecNodeList(thing,thing->x,thing->y);
      thing->touching_sectorlist = _g->sector_list; // Attach to Thing's mobj_t
      _g->sector_list = NULL; // clear for next time
    }

  // link into blockmap
  if (!(thing->flags & MF_NOBLOCKMAP))
    {
      // inert things don't need to be in blockmap
      int blockx = (thing->x - _g->bmaporgx)>>MAPBLOCKSHIFT;
      int blocky = (thing->y - _g->bmaporgy)>>MAPBLOCKSHIFT;
      if (blockx>=0 && blockx < _g->bmapwidth && blocky>=0 && blocky < _g->bmapheight)
        {
        // killough 8/11/98: simpler scheme using pointer-to-pointer prev
        // pointers, allows head nodes to be treated like everything else

#if GBADOOM_COMPACT_BLOCKLINKS
        mobj_t **link = &_g->blocklinks[0];
#else
        mobj_t **link = &_g->blocklinks[blocky*_g->bmapwidth+blockx];
#endif
        mobj_t *bnext = *link;
        if ((thing->bnext = bnext))
          bnext->bprev = &thing->bnext;
        thing->bprev = link;
        *link = thing;
      }
      else        // thing is off the map
        thing->bnext = NULL, thing->bprev = NULL;
    }
}

//
// BLOCK MAP ITERATORS
// For each line/thing in the given mapblock,
// call the passed PIT_* function.
// If the function returns false,
// exit with false without checking anything else.
//

//
// P_BlockLinesIterator
// The validcount flags are used to avoid checking lines
// that are marked in multiple mapblocks,
// so increment validcount before the first call
// to P_BlockLinesIterator, then make one or more calls
// to it.
//
// killough 5/3/98: reformatted, cleaned up

boolean P_BlockLinesIterator(int x, int y, boolean func(const line_t*))
{

    if (x<0 || y<0 || x>=_g->bmapwidth || y>=_g->bmapheight)
        return true;

    if (_g->blockmap_is_compact)
        return P_BlockLinesIteratorCompact(x, y, func);

    const int offset = _g->blockmap[y*_g->bmapwidth+x];
    const short* list = _g->blockmaplump+offset;     // original was reading         // phares


    // delmiting 0 as linedef 0     // phares

    // killough 1/31/98: for compatibility we need to use the old method.
    // Most demos go out of sync, and maybe other problems happen, if we
    // don't consider linedef 0. For safety this should be qualified.

    if (_g->blockmap_has_leading_zero && *list == 0)
      list++;   // skip classic 0 starting delimiter when present

    const int vcount = _g->validcount;

    for ( ; *list != -1 ; list++)                                   // phares
    {
        const int lineno = *list;

        if (!P_VisitBlockLineByIndex(lineno, func, vcount))
            return false;
    }

    return true;  // everything was checked
}

//
// P_BlockThingsIterator
//
// killough 5/3/98: reformatted, cleaned up

boolean P_BlockThingsIterator(int x, int y, boolean func(mobj_t*))
{
  mobj_t *mobj;
  if (!(x<0 || y<0 || x>=_g->bmapwidth || y>=_g->bmapheight))
#if GBADOOM_COMPACT_BLOCKLINKS
  {
    const int wanted = y * _g->bmapwidth + x;
    for (mobj = _g->blocklinks[0]; mobj; mobj = mobj->bnext)
      if (P_BlockIndexForThing(mobj) == wanted && !func(mobj))
        return false;
  }
#else
    for (mobj = _g->blocklinks[y*_g->bmapwidth+x]; mobj; mobj = mobj->bnext)
      if (!func(mobj))
        return false;
#endif
  return true;
}

//
// INTERCEPT ROUTINES
//

// Check for limit and double size if necessary -- killough
static boolean check_intercept(void)
{
    size_t offset = _g->intercept_p - _g->intercepts;

    return (offset < MAXINTERCEPTS);
}


// PIT_AddLineIntercepts.
// Looks for lines in the given block
// that intercept the given trace
// to add to the intercepts list.
//
// A line is crossed if its endpoints
// are on opposite sides of the trace.
//
// killough 5/3/98: reformatted, cleaned up

boolean PIT_AddLineIntercepts(const line_t *ld)
{
  int       s1;
  int       s2;
  fixed_t   frac;
  divline_t dl;

  // avoid precision problems with two routines
  if (_g->trace.dx >  FRACUNIT*16 || _g->trace.dy >  FRACUNIT*16 ||
      _g->trace.dx < -FRACUNIT*16 || _g->trace.dy < -FRACUNIT*16)
    {
      s1 = P_PointOnDivlineSide (LN_V1(ld)->x, LN_V1(ld)->y, &_g->trace);
      s2 = P_PointOnDivlineSide (LN_V2(ld)->x, LN_V2(ld)->y, &_g->trace);
    }
  else
    {
      s1 = P_PointOnLineSide (_g->trace.x, _g->trace.y, ld);
      s2 = P_PointOnLineSide (_g->trace.x+_g->trace.dx, _g->trace.y+_g->trace.dy, ld);
    }

  if (s1 == s2)
    return true;        // line isn't crossed

  // hit the line
  P_MakeDivline(ld, &dl);
  frac = P_InterceptVector2(&_g->trace, &dl);

  if (frac < 0)
    return true;        // behind source

  if(!check_intercept())
    return false;

  _g->intercept_p->frac = frac;
  _g->intercept_p->isaline = true;
  _g->intercept_p->d.line = ld;
  _g->intercept_p++;

  return true;  // continue
}

//
// PIT_AddThingIntercepts
//
// killough 5/3/98: reformatted, cleaned up

boolean PIT_AddThingIntercepts(mobj_t *thing)
{
  fixed_t   x1, y1;
  fixed_t   x2, y2;
  int       s1, s2;
  divline_t dl;
  fixed_t   frac;

  // check a corner to corner crossection for hit
  if ((_g->trace.dx ^ _g->trace.dy) > 0)
    {
      x1 = thing->x - thing->radius;
      y1 = thing->y + thing->radius;
      x2 = thing->x + thing->radius;
      y2 = thing->y - thing->radius;
    }
  else
    {
      x1 = thing->x - thing->radius;
      y1 = thing->y - thing->radius;
      x2 = thing->x + thing->radius;
      y2 = thing->y + thing->radius;
    }

  s1 = P_PointOnDivlineSide (x1, y1, &_g->trace);
  s2 = P_PointOnDivlineSide (x2, y2, &_g->trace);

  if (s1 == s2)
    return true;                // line isn't crossed

  dl.x = x1;
  dl.y = y1;
  dl.dx = x2-x1;
  dl.dy = y2-y1;

  frac = P_InterceptVector2(&_g->trace, &dl);

  if (frac < 0)
    return true;                // behind source

  if(!check_intercept())
      return false;

  _g->intercept_p->frac = frac;
  _g->intercept_p->isaline = false;
  _g->intercept_p->d.thing = thing;
  _g->intercept_p++;

  return true;          // keep going
}

//
// P_TraverseIntercepts
// Returns true if the traverser function returns true
// for all lines.
//
// killough 5/3/98: reformatted, cleaned up

boolean P_TraverseIntercepts(traverser_t func, fixed_t maxfrac)
{
  intercept_t *in = NULL;
  int count = _g->intercept_p - _g->intercepts;
  while (count--)
    {
      fixed_t dist = INT_MAX;
      intercept_t *scan;
      for (scan = _g->intercepts; scan < _g->intercept_p; scan++)
        if (scan->frac < dist)
          dist = (in=scan)->frac;
      if (dist > maxfrac)
        return true;    // checked everything in range
      if (!func(in))
        return false;           // don't bother going farther
      in->frac = INT_MAX;
    }
  return true;                  // everything was traversed
}

//
// P_PathTraverse
// Traces a line from x1,y1 to x2,y2,
// calling the traverser function for each.
// Returns true if the traverser function returns true
// for all lines.
//
// killough 5/3/98: reformatted, cleaned up

boolean P_PathTraverse(fixed_t x1, fixed_t y1, fixed_t x2, fixed_t y2,
                       int flags, boolean trav(intercept_t *))
{
  fixed_t xt1, yt1;
  fixed_t xt2, yt2;
  fixed_t xstep, ystep;
  fixed_t partial;
  fixed_t xintercept, yintercept;
  int     mapx, mapy;
  int     mapxstep, mapystep;
  int     count;

  _g->validcount++;
  _g->intercept_p = _g->intercepts;

  if (!((x1-_g->bmaporgx)&(MAPBLOCKSIZE-1)))
    x1 += FRACUNIT;     // don't side exactly on a line

  if (!((y1-_g->bmaporgy)&(MAPBLOCKSIZE-1)))
    y1 += FRACUNIT;     // don't side exactly on a line

  _g->trace.x = x1;
  _g->trace.y = y1;
  _g->trace.dx = x2 - x1;
  _g->trace.dy = y2 - y1;

  x1 -= _g->bmaporgx;
  y1 -= _g->bmaporgy;
  xt1 = x1>>MAPBLOCKSHIFT;
  yt1 = y1>>MAPBLOCKSHIFT;

  x2 -= _g->bmaporgx;
  y2 -= _g->bmaporgy;
  xt2 = x2>>MAPBLOCKSHIFT;
  yt2 = y2>>MAPBLOCKSHIFT;

  if (xt2 > xt1)
    {
      mapxstep = 1;
      partial = FRACUNIT - ((x1>>MAPBTOFRAC)&(FRACUNIT-1));
      ystep = FixedDiv (y2-y1,D_abs(x2-x1));
    }
  else
    if (xt2 < xt1)
      {
        mapxstep = -1;
        partial = (x1>>MAPBTOFRAC)&(FRACUNIT-1);
        ystep = FixedDiv (y2-y1,D_abs(x2-x1));
      }
    else
      {
        mapxstep = 0;
        partial = FRACUNIT;
        ystep = 256*FRACUNIT;
      }

  yintercept = (y1>>MAPBTOFRAC) + FixedMul(partial, ystep);

  if (yt2 > yt1)
    {
      mapystep = 1;
      partial = FRACUNIT - ((y1>>MAPBTOFRAC)&(FRACUNIT-1));
      xstep = FixedDiv (x2-x1,D_abs(y2-y1));
    }
  else
    if (yt2 < yt1)
      {
        mapystep = -1;
        partial = (y1>>MAPBTOFRAC)&(FRACUNIT-1);
        xstep = FixedDiv (x2-x1,D_abs(y2-y1));
      }
    else
      {
        mapystep = 0;
        partial = FRACUNIT;
        xstep = 256*FRACUNIT;
      }

  xintercept = (x1>>MAPBTOFRAC) + FixedMul (partial, xstep);

  // Step through map blocks.
  // Count is present to prevent a round off error
  // from skipping the break.

  mapx = xt1;
  mapy = yt1;

  for (count = 0; count < 64; count++)
    {
      if (flags & PT_ADDLINES)
        if (!P_BlockLinesIterator(mapx, mapy,PIT_AddLineIntercepts))
          return false; // early out

      if (flags & PT_ADDTHINGS)
        if (!P_BlockThingsIterator(mapx, mapy,PIT_AddThingIntercepts))
          return false; // early out

      if (mapx == xt2 && mapy == yt2)
        break;

      if ((yintercept >> FRACBITS) == mapy)
        {
          yintercept += ystep;
          mapx += mapxstep;
        }
      else
        if ((xintercept >> FRACBITS) == mapx)
          {
            xintercept += xstep;
            mapy += mapystep;
          }
    }

  // go through the sorted list
  return P_TraverseIntercepts(trav, FRACUNIT);
}
