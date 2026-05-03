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
 *  Do all the WAD I/O, get map description,
 *  set up initial state and misc. LUTs.
 *
 *-----------------------------------------------------------------------------*/

#include <math.h>
#include <string.h>

#include "doomstat.h"
#include "m_bbox.h"
#include "g_game.h"
#include "w_wad.h"
#include "r_main.h"
#include "r_things.h"
#include "p_maputl.h"
#include "p_map.h"
#include "p_setup.h"
#include "p_spec.h"
#include "p_tick.h"
#include "p_enemy.h"
#include "s_sound.h"
#include "lprintf.h" //jff 10/6/98 for debug outputs
#include "v_video.h"

#include "global_data.h"

int R_LoadTextureByName(const char* tex_name);

#define GBADOOM_HUFF_LUMP_MAGIC 0x30465548u /* "HUF0" */
#define GBADOOM_HUFF_HEADER_SIZE 16u
#define GBADOOM_HUFF_MAX_SYMBOLS 256
#define GBADOOM_HUFF_MAX_NODES (GBADOOM_HUFF_MAX_SYMBOLS * 2)
#define GBADOOM_NODE_COMPACT_MAGIC 0x3043444Eu /* "NDC0" */
#define GBADOOM_NODE_COMPACT_HEADER_SIZE 16u
#define GBADOOM_NODE_COMPACT_ENTRY_SIZE 16u
#define GBADOOM_SSECTOR_COMPACT_MAGIC 0x30435353u /* "SSC0" */
#define GBADOOM_SSECTOR_COMPACT_HEADER_SIZE 8u
#define GBADOOM_SIDEDEF_VAR_MAGIC 0x30564453u /* "SDV0" */
#define GBADOOM_SIDEDEF_VAR_HEADER_SIZE 8u
#define GBADOOM_BLOCKMAP_COMPACT_MAGIC 0x30434D42u /* "BMC0" */
#define GBADOOM_BLOCKMAP_COMPACT_HEADER_SIZE 16u

typedef struct
{
    const byte* payload;
    unsigned int payload_bits;
    unsigned int bit_pos;
    unsigned int raw_size;
    unsigned int raw_pos;
    short child[GBADOOM_HUFF_MAX_NODES][2];
    short symbol[GBADOOM_HUFF_MAX_NODES];
    int node_count;
} huff_lump_reader_t;

typedef struct
{
    short floorheight;
    short ceilingheight;
    short floorpic;
    short ceilingpic;
    unsigned char lightlevel;
    short special;
    short tag;
} PACKEDATTR mapcompactsector_t;

typedef struct
{
  short x;
  short y;
  short dx;
  short dy;
  unsigned short children[2];
  unsigned char bbox_q[4];
} PACKEDATTR mapcompactnode_t;

static unsigned int P_ReadU32LE(const byte* p)
{
    return ((unsigned int)p[0])
        | ((unsigned int)p[1] << 8)
        | ((unsigned int)p[2] << 16)
        | ((unsigned int)p[3] << 24);
}

static unsigned int P_ReadU16LE(const byte* p)
{
    return ((unsigned int)p[0])
        | ((unsigned int)p[1] << 8);
}

static void P_HuffResetReader(huff_lump_reader_t* reader)
{
    if (reader == NULL)
        I_Error("P_HuffResetReader: NULL reader");

    reader->bit_pos = 0;
    reader->raw_pos = 0;
}

static boolean P_HuffReadBit(huff_lump_reader_t* reader, unsigned int* bit)
{
    unsigned int byte_index;
    unsigned int bit_index;

    if (reader->bit_pos >= reader->payload_bits)
        return false;

    byte_index = reader->bit_pos >> 3;
    bit_index = 7u - (reader->bit_pos & 7u);

    *bit = ((unsigned int)reader->payload[byte_index] >> bit_index) & 1u;
    reader->bit_pos++;
    return true;
}

static boolean P_HuffReadSymbol(huff_lump_reader_t* reader, byte* value)
{
    int node = 0;
    unsigned int bit;

    if (reader->raw_pos >= reader->raw_size)
        return false;

    while (reader->symbol[node] < 0)
    {
        if (!P_HuffReadBit(reader, &bit))
            return false;

        node = reader->child[node][bit];
        if (node < 0 || node >= reader->node_count)
            return false;
    }

    *value = (byte)reader->symbol[node];
    reader->raw_pos++;
    return true;
}

static void P_HuffReadBytesOrError(
    huff_lump_reader_t* reader, void* dst, unsigned int count, const char* context, int lump)
{
    unsigned int i;
    byte* out = (byte*)dst;

    for (i = 0; i < count; i++)
    {
        if (!P_HuffReadSymbol(reader, &out[i]))
            I_Error("%s: failed to decode Huffman lump %d", context, lump);
    }
}

static void P_HuffFinishOrError(const huff_lump_reader_t* reader, const char* context, int lump)
{
    if (reader->raw_pos != reader->raw_size)
        I_Error("%s: incomplete Huffman decode for lump %d", context, lump);
}

static byte P_ReadPackedByteOrError(
    boolean is_huffman,
    huff_lump_reader_t* huff,
    const byte** cursor,
    const byte* end,
    const char* context,
    int lump)
{
    byte value;

    if (is_huffman)
    {
        P_HuffReadBytesOrError(huff, &value, 1, context, lump);
        return value;
    }

    if (cursor == NULL || *cursor == NULL || end == NULL || *cursor >= end)
        I_Error("%s: truncated packed lump %d", context, lump);

    value = **cursor;
    (*cursor)++;
    return value;
}

static unsigned int P_ReadPackedU16OrError(
    boolean is_huffman,
    huff_lump_reader_t* huff,
    const byte** cursor,
    const byte* end,
    const char* context,
    int lump)
{
    byte lo = P_ReadPackedByteOrError(is_huffman, huff, cursor, end, context, lump);
    byte hi = P_ReadPackedByteOrError(is_huffman, huff, cursor, end, context, lump);
    return ((unsigned int)lo) | ((unsigned int)hi << 8);
}

static int P_ReadPackedS16OrError(
    boolean is_huffman,
    huff_lump_reader_t* huff,
    const byte** cursor,
    const byte* end,
    const char* context,
    int lump)
{
    unsigned int value = P_ReadPackedU16OrError(is_huffman, huff, cursor, end, context, lump);
    if (value & 0x8000u)
        return (int)value - 0x10000;
    return (int)value;
}

static void P_HuffInsertCanonicalCode(
    huff_lump_reader_t* reader, unsigned int code, unsigned int code_len, unsigned int symbol)
{
    int node = 0;
    int bit_index;

    if (code_len == 0)
        I_Error("P_HuffInsertCanonicalCode: invalid code length");

    for (bit_index = (int)code_len - 1; bit_index >= 0; bit_index--)
    {
        int bit = (int)((code >> bit_index) & 1u);
        int child = reader->child[node][bit];

        if (bit_index == 0)
        {
            if (child != -1)
                I_Error("P_HuffInsertCanonicalCode: duplicate Huffman leaf");
            if (reader->node_count >= GBADOOM_HUFF_MAX_NODES)
                I_Error("P_HuffInsertCanonicalCode: Huffman tree overflow");

            child = reader->node_count++;
            reader->child[child][0] = -1;
            reader->child[child][1] = -1;
            reader->symbol[child] = (short)symbol;
            reader->child[node][bit] = (short)child;
        }
        else
        {
            if (child == -1)
            {
                if (reader->node_count >= GBADOOM_HUFF_MAX_NODES)
                    I_Error("P_HuffInsertCanonicalCode: Huffman tree overflow");

                child = reader->node_count++;
                reader->child[child][0] = -1;
                reader->child[child][1] = -1;
                reader->symbol[child] = -1;
                reader->child[node][bit] = (short)child;
            }
            else if (reader->symbol[child] >= 0)
            {
                I_Error("P_HuffInsertCanonicalCode: invalid canonical prefix");
            }

            node = child;
        }
    }
}

static boolean P_HuffInitLumpReader(int lump, huff_lump_reader_t* reader)
{
    const byte* data;
    int lump_size;
    unsigned int raw_size;
    unsigned int payload_size;
    unsigned int symbol_count;
    unsigned int max_code_len;
    unsigned int table_bytes;
    unsigned int data_offset;
    unsigned int code;
    unsigned int prev_len;
    unsigned int sorted_symbols[GBADOOM_HUFF_MAX_SYMBOLS];
    unsigned char code_lengths[GBADOOM_HUFF_MAX_SYMBOLS];
    unsigned char seen_symbols[GBADOOM_HUFF_MAX_SYMBOLS];
    unsigned int len;
    unsigned int symbol;
    unsigned int used = 0;
    int i;

    if (reader == NULL)
        I_Error("P_HuffInitLumpReader: NULL reader");

    data = W_CacheLumpNum(lump);
    lump_size = W_LumpLength(lump);
    if (data == NULL || lump_size < (int)GBADOOM_HUFF_HEADER_SIZE)
        return false;

    if (P_ReadU32LE(data) != GBADOOM_HUFF_LUMP_MAGIC)
        return false;

    raw_size = P_ReadU32LE(data + 4);
    payload_size = P_ReadU32LE(data + 8);
    symbol_count = (unsigned int)data[12] | ((unsigned int)data[13] << 8);
    max_code_len = (unsigned int)data[14];

    if (symbol_count == 0 || symbol_count > GBADOOM_HUFF_MAX_SYMBOLS)
        I_Error("P_HuffInitLumpReader: invalid symbol count in lump %d", lump);
    if (max_code_len == 0 || max_code_len > 31)
        I_Error("P_HuffInitLumpReader: invalid max code len in lump %d", lump);

    table_bytes = symbol_count * 2u;
    data_offset = GBADOOM_HUFF_HEADER_SIZE + table_bytes;
    if ((unsigned int)lump_size < data_offset || (unsigned int)lump_size - data_offset < payload_size)
        I_Error("P_HuffInitLumpReader: malformed Huffman lump %d", lump);

    memset(code_lengths, 0, sizeof(code_lengths));
    memset(seen_symbols, 0, sizeof(seen_symbols));

    data_offset = GBADOOM_HUFF_HEADER_SIZE;
    for (i = 0; i < (int)symbol_count; i++)
    {
        symbol = data[data_offset++];
        len = data[data_offset++];

        if (seen_symbols[symbol])
            I_Error("P_HuffInitLumpReader: duplicate symbol in lump %d", lump);
        if (len == 0 || len > max_code_len)
            I_Error("P_HuffInitLumpReader: invalid code length in lump %d", lump);

        seen_symbols[symbol] = 1;
        code_lengths[symbol] = (unsigned char)len;
    }

    for (len = 1; len <= max_code_len; len++)
    {
        for (symbol = 0; symbol < GBADOOM_HUFF_MAX_SYMBOLS; symbol++)
        {
            if (code_lengths[symbol] == len)
                sorted_symbols[used++] = symbol;
        }
    }
    if (used != symbol_count)
        I_Error("P_HuffInitLumpReader: inconsistent Huffman table in lump %d", lump);

    for (i = 0; i < GBADOOM_HUFF_MAX_NODES; i++)
    {
        reader->child[i][0] = -1;
        reader->child[i][1] = -1;
        reader->symbol[i] = -1;
    }
    reader->node_count = 1;

    code = 0;
    prev_len = code_lengths[sorted_symbols[0]];
    for (i = 0; i < (int)used; i++)
    {
        symbol = sorted_symbols[i];
        len = code_lengths[symbol];

        if (i == 0)
        {
            code = 0;
            prev_len = len;
        }
        else
        {
            code <<= (len - prev_len);
            prev_len = len;
        }

        P_HuffInsertCanonicalCode(reader, code, len, symbol);
        code++;
    }

    reader->payload = data + GBADOOM_HUFF_HEADER_SIZE + table_bytes;
    reader->payload_bits = payload_size * 8u;
    reader->raw_size = raw_size;
    P_HuffResetReader(reader);
    return true;
}

//
// P_LoadVertexes
//
// killough 5/3/98: reformatted, cleaned up
//
static void P_LoadVertexes (int lump)
{
  // Determine number of lumps:
  //  total lump length / vertex record length.
  _g->numvertexes = W_LumpLength(lump) / sizeof(vertex_t);

  // Allocate zone memory for buffer.
  _g->vertexes = W_CacheLumpNum(lump);

}

//
// P_LoadSegs
//
// killough 5/3/98: reformatted, cleaned up

static void P_LoadSegs (int lump)
{
    int numsegs = W_LumpLength(lump) / sizeof(seg_t);
    _g->segs = (const seg_t *)W_CacheLumpNum(lump);

    if (!numsegs)
      I_Error("P_LoadSegs: no segs in level");
}

//
// P_LoadSubsectors
//
// killough 5/3/98: reformatted, cleaned up

static void P_LoadSubsectors (int lump)
{
  const byte *raw_data = NULL;
  const mapsubsector_t *data = NULL;
  int lump_size;
  boolean is_huffman = false;
  boolean is_compact = false;
  unsigned int compact_count = 0;
  huff_lump_reader_t huff;
  int  i;

  if (P_HuffInitLumpReader(lump, &huff))
  {
    is_huffman = true;
    lump_size = (int)huff.raw_size;
  }
  else
  {
    lump_size = W_LumpLength(lump);
    raw_data = W_CacheLumpNum(lump);
    data = (const mapsubsector_t *)raw_data;
  }

  if (lump_size >= (int)GBADOOM_SSECTOR_COMPACT_HEADER_SIZE)
  {
    byte header[GBADOOM_SSECTOR_COMPACT_HEADER_SIZE];
    unsigned int expected_size;

    if (is_huffman)
    {
      P_HuffResetReader(&huff);
      P_HuffReadBytesOrError(&huff, header, sizeof(header), "P_LoadSubsectors", lump);
    }
    else
    {
      memcpy(header, raw_data, sizeof(header));
    }

    if (P_ReadU32LE(header) == GBADOOM_SSECTOR_COMPACT_MAGIC)
    {
      compact_count = P_ReadU16LE(header + 4);
      expected_size = GBADOOM_SSECTOR_COMPACT_HEADER_SIZE + ((compact_count + 1u) * 2u);
      if (compact_count > 0u && expected_size == (unsigned int)lump_size)
      {
        is_compact = true;
      }
    }
  }

  if (is_compact)
  {
    _g->numsubsectors = (int)compact_count;
    _g->subsectors = Z_Calloc(_g->numsubsectors, sizeof(subsector_t), PU_LEVEL, 0);

    if (!_g->numsubsectors || !_g->subsectors)
      I_Error("P_LoadSubsectors: no subsectors in level");

    if (is_huffman)
    {
      byte header[GBADOOM_SSECTOR_COMPACT_HEADER_SIZE];
      unsigned int previous_firstseg;

      P_HuffResetReader(&huff);
      P_HuffReadBytesOrError(&huff, header, sizeof(header), "P_LoadSubsectors", lump);
      if (P_ReadU32LE(header) != GBADOOM_SSECTOR_COMPACT_MAGIC
          || P_ReadU16LE(header + 4) != compact_count)
      {
        I_Error("P_LoadSubsectors: invalid compact header in lump %d", lump);
      }

      previous_firstseg = P_ReadPackedU16OrError(true, &huff, NULL, NULL, "P_LoadSubsectors", lump);
      for (i = 0; i < _g->numsubsectors; i++)
      {
        unsigned int next_firstseg =
            P_ReadPackedU16OrError(true, &huff, NULL, NULL, "P_LoadSubsectors", lump);
        unsigned int numsegs;

        if (next_firstseg < previous_firstseg)
          I_Error("P_LoadSubsectors: invalid compact subsector range in lump %d", lump);

        numsegs = next_firstseg - previous_firstseg;
        if (numsegs > 0xFFFFu)
          I_Error("P_LoadSubsectors: compact subsector span overflow in lump %d", lump);

        _g->subsectors[i].firstline = (unsigned short)previous_firstseg;
        _g->subsectors[i].numlines = (unsigned short)numsegs;
        previous_firstseg = next_firstseg;
      }

      P_HuffFinishOrError(&huff, "P_LoadSubsectors", lump);
    }
    else
    {
      const byte *first_data = raw_data + GBADOOM_SSECTOR_COMPACT_HEADER_SIZE;

      for (i = 0; i < _g->numsubsectors; i++)
      {
        unsigned int firstseg = P_ReadU16LE(first_data + (i * 2));
        unsigned int next_firstseg = P_ReadU16LE(first_data + ((i + 1) * 2));
        unsigned int numsegs;

        if (next_firstseg < firstseg)
          I_Error("P_LoadSubsectors: invalid compact subsector range in lump %d", lump);

        numsegs = next_firstseg - firstseg;
        _g->subsectors[i].firstline = (unsigned short)firstseg;
        _g->subsectors[i].numlines = (unsigned short)numsegs;
      }
    }

    return;
  }

  if (is_huffman)
    P_HuffResetReader(&huff);

  if (lump_size % (int)sizeof(mapsubsector_t) != 0)
    I_Error("P_LoadSubsectors: invalid lump size %d", lump_size);

  _g->numsubsectors = lump_size / (int)sizeof(mapsubsector_t);
  _g->subsectors = Z_Calloc(_g->numsubsectors,sizeof(subsector_t),PU_LEVEL,0);

  if (((!is_huffman) && (!data)) || (!_g->numsubsectors))
    I_Error("P_LoadSubsectors: no subsectors in level");

  for (i=0; i<_g->numsubsectors; i++)
  {
    mapsubsector_t decoded_subsector;
    const mapsubsector_t* map_subsector;

    if (is_huffman)
    {
      P_HuffReadBytesOrError(&huff, &decoded_subsector, sizeof(decoded_subsector), "P_LoadSubsectors", lump);
      map_subsector = &decoded_subsector;
    }
    else
    {
      map_subsector = data + i;
    }

    _g->subsectors[i].numlines  = (unsigned short)SHORT(map_subsector->numsegs );
    _g->subsectors[i].firstline = (unsigned short)SHORT(map_subsector->firstseg);
  }

  if (is_huffman)
    P_HuffFinishOrError(&huff, "P_LoadSubsectors", lump);
}

//
// P_LoadSectors
//
// killough 5/3/98: reformatted, cleaned up

static void P_LoadSectors (int lump)
{
  const byte *data = NULL; // cph - const*
  int lump_size;
  int  i;
  boolean is_huffman = false;
  boolean can_be_vanilla;
  boolean can_be_compact;
  boolean is_compact = false;
  huff_lump_reader_t huff;

  if (P_HuffInitLumpReader(lump, &huff))
  {
    is_huffman = true;
    lump_size = (int)huff.raw_size;
  }
  else
  {
    data = W_CacheLumpNum(lump);
    lump_size = W_LumpLength(lump);
  }

  can_be_vanilla = (lump_size % (int)sizeof(mapsector_t) == 0);
  can_be_compact = (lump_size % (int)sizeof(mapcompactsector_t) == 0);

  if (!can_be_vanilla && !can_be_compact)
    I_Error("P_LoadSectors: invalid lump size %d", lump_size);

  if (can_be_compact && !can_be_vanilla)
  {
    is_compact = true;
  }
  else if (can_be_vanilla && !can_be_compact)
  {
    is_compact = false;
  }
  else
  {
    int vanilla_count = lump_size / (int)sizeof(mapsector_t);
    int compact_count = lump_size / (int)sizeof(mapcompactsector_t);
    int vanilla_bad = 0;
    int compact_bad = 0;

    if (is_huffman)
    {
      P_HuffResetReader(&huff);
      for (i = 0; i < vanilla_count; i++)
      {
        mapsector_t ms;
        int floor_lump;
        int ceiling_lump;
        P_HuffReadBytesOrError(&huff, &ms, sizeof(ms), "P_LoadSectors", lump);
        floor_lump = W_CheckNumForName(ms.floorpic);
        ceiling_lump = W_CheckNumForName(ms.ceilingpic);
        if (floor_lump < _g->firstflat || floor_lump >= _g->firstflat + _g->numflats)
          vanilla_bad++;
        if (ceiling_lump < _g->firstflat || ceiling_lump >= _g->firstflat + _g->numflats)
          vanilla_bad++;
      }
      P_HuffFinishOrError(&huff, "P_LoadSectors", lump);

      P_HuffResetReader(&huff);
      for (i = 0; i < compact_count; i++)
      {
        mapcompactsector_t ms;
        int floor_num;
        int ceiling_num;
        P_HuffReadBytesOrError(&huff, &ms, sizeof(ms), "P_LoadSectors", lump);
        floor_num = SHORT(ms.floorpic);
        ceiling_num = SHORT(ms.ceilingpic);
        if ((unsigned int)floor_num >= (unsigned int)_g->numflats)
          compact_bad++;
        if ((unsigned int)ceiling_num >= (unsigned int)_g->numflats)
          compact_bad++;
      }
      P_HuffFinishOrError(&huff, "P_LoadSectors", lump);
    }
    else
    {
      for (i = 0; i < vanilla_count; i++)
      {
        const mapsector_t *ms = (const mapsector_t *)data + i;
        int floor_lump = W_CheckNumForName(ms->floorpic);
        int ceiling_lump = W_CheckNumForName(ms->ceilingpic);
        if (floor_lump < _g->firstflat || floor_lump >= _g->firstflat + _g->numflats)
          vanilla_bad++;
        if (ceiling_lump < _g->firstflat || ceiling_lump >= _g->firstflat + _g->numflats)
          vanilla_bad++;
      }

      for (i = 0; i < compact_count; i++)
      {
        const mapcompactsector_t *ms = (const mapcompactsector_t *)data + i;
        int floor_num = SHORT(ms->floorpic);
        int ceiling_num = SHORT(ms->ceilingpic);
        if ((unsigned int)floor_num >= (unsigned int)_g->numflats)
          compact_bad++;
        if ((unsigned int)ceiling_num >= (unsigned int)_g->numflats)
          compact_bad++;
      }
    }

    is_compact = (compact_bad < vanilla_bad);
    lprintf(LO_WARN,
        "P_LoadSectors: ambiguous sector lump (%d bytes), chose %s (vanilla bad=%d compact bad=%d)\n",
        lump_size,
        is_compact ? "COMPACT" : "VANILLA",
        vanilla_bad,
        compact_bad);
  }

  if (is_compact)
    _g->numsectors = lump_size / (int)sizeof(mapcompactsector_t);
  else
    _g->numsectors = lump_size / (int)sizeof(mapsector_t);

  _g->sectors = Z_Calloc (_g->numsectors,sizeof(sector_t),PU_LEVEL,0);

  if (((!is_huffman) && (!data)) || (!_g->numsectors))
    I_Error("P_LoadSectors: no sectors in level");

  if (is_huffman)
    P_HuffResetReader(&huff);

  for (i=0; i<_g->numsectors; i++)
    {
      sector_t *ss = _g->sectors + i;
      int floorpic;
      int ceilingpic;

      if (is_compact)
      {
        mapcompactsector_t decoded_sector;
        const mapcompactsector_t *ms;

        if (is_huffman)
        {
          P_HuffReadBytesOrError(&huff, &decoded_sector, sizeof(decoded_sector), "P_LoadSectors", lump);
          ms = &decoded_sector;
        }
        else
        {
          ms = (const mapcompactsector_t *) data + i;
        }

        floorpic = SHORT(ms->floorpic);
        ceilingpic = SHORT(ms->ceilingpic);
        if ((unsigned int)floorpic >= (unsigned int)_g->numflats)
          floorpic = 0;
        if ((unsigned int)ceilingpic >= (unsigned int)_g->numflats)
          ceilingpic = 0;

        ss->floorheight = SHORT(ms->floorheight)<<FRACBITS;
        ss->ceilingheight = SHORT(ms->ceilingheight)<<FRACBITS;
        ss->floorpic = floorpic;
        ss->ceilingpic = ceilingpic;
        ss->lightlevel = (short)ms->lightlevel;
        ss->special = SHORT(ms->special);
        ss->oldspecial = SHORT(ms->special);
        ss->tag = SHORT(ms->tag);
      }
      else
      {
        mapsector_t decoded_sector;
        const mapsector_t *ms;

        if (is_huffman)
        {
          P_HuffReadBytesOrError(&huff, &decoded_sector, sizeof(decoded_sector), "P_LoadSectors", lump);
          ms = &decoded_sector;
        }
        else
        {
          ms = (const mapsector_t *) data + i;
        }

        ss->floorheight = SHORT(ms->floorheight)<<FRACBITS;
        ss->ceilingheight = SHORT(ms->ceilingheight)<<FRACBITS;
        ss->floorpic = R_FlatNumForName(ms->floorpic);
        ss->ceilingpic = R_FlatNumForName(ms->ceilingpic);
        ss->lightlevel = SHORT(ms->lightlevel);
        ss->special = SHORT(ms->special);
        ss->oldspecial = SHORT(ms->special);
        ss->tag = SHORT(ms->tag);
      }

      ss->thinglist = NULL;
      ss->touching_thinglist = NULL;            // phares 3/14/98
    }

  if (is_huffman)
    P_HuffFinishOrError(&huff, "P_LoadSectors", lump);
}


//
// P_LoadNodes
//
// killough 5/3/98: reformatted, cleaned up

static void P_LoadNodes (int lump)
{
  const byte *data = W_CacheLumpNum(lump);
  int lump_size = W_LumpLength(lump);

  nodes = NULL;
  nodes_are_compact = false;
  nodes_compact = NULL;
  memset(nodes_root_bbox, 0, sizeof(nodes_root_bbox));

  if (data != NULL && lump_size >= (int)GBADOOM_NODE_COMPACT_HEADER_SIZE
      && P_ReadU32LE(data) == GBADOOM_NODE_COMPACT_MAGIC)
  {
    unsigned int compact_count = (unsigned int)data[4] | ((unsigned int)data[5] << 8);
    unsigned int expected_size = GBADOOM_NODE_COMPACT_HEADER_SIZE
        + compact_count * GBADOOM_NODE_COMPACT_ENTRY_SIZE;

    if ((unsigned int)lump_size == expected_size)
    {
      numnodes = (int)compact_count;
      nodes_are_compact = true;
      nodes_compact = data + GBADOOM_NODE_COMPACT_HEADER_SIZE;
      nodes_root_bbox[BOXTOP] = (short)((unsigned short)data[6] | ((unsigned short)data[7] << 8));
      nodes_root_bbox[BOXBOTTOM] = (short)((unsigned short)data[8] | ((unsigned short)data[9] << 8));
      nodes_root_bbox[BOXLEFT] = (short)((unsigned short)data[10] | ((unsigned short)data[11] << 8));
      nodes_root_bbox[BOXRIGHT] = (short)((unsigned short)data[12] | ((unsigned short)data[13] << 8));
    }
    else
    {
      I_Error("P_LoadNodes: malformed compact nodes lump size %d", lump_size);
    }
  }
  else
  {
    numnodes = lump_size / (int)sizeof(mapnode_t);
    nodes = (const mapnode_t *)data; // cph - wad lump handling updated
  }

  if (((!nodes_are_compact) && (!nodes)) || (!numnodes))
  {
    // allow trivial maps
    if (_g->numsubsectors == 1)
      lprintf(LO_INFO,
          "P_LoadNodes: trivial map (no nodes, one subsector)\n");
    else
      I_Error("P_LoadNodes: no nodes in level");
  }
}


static int P_CountThings(int lump)
{
    int numthings;
    int lump_size;
    int spawnableThings = 0;
    const mapthing_t* data = NULL;
    boolean is_huffman = false;
    huff_lump_reader_t huff;

    if (P_HuffInitLumpReader(lump, &huff))
    {
        is_huffman = true;
        lump_size = (int)huff.raw_size;
    }
    else
    {
        data = (const mapthing_t*)W_CacheLumpNum(lump);
        lump_size = W_LumpLength(lump);
    }

    if (lump_size % (int)sizeof(mapthing_t) != 0)
        I_Error("P_LoadThings: invalid lump size %d", lump_size);

    numthings = lump_size / (int)sizeof(mapthing_t);

    if (((!is_huffman) && (!data)) || (!numthings))
        I_Error("P_LoadThings: no things in level");

    for (int i = 0; i < numthings; i++)
    {
        mapthing_t decoded_thing;
        const mapthing_t* mt;

        if (is_huffman)
        {
            P_HuffReadBytesOrError(&huff, &decoded_thing, sizeof(decoded_thing), "P_LoadThings", lump);
            mt = &decoded_thing;
        }
        else
        {
            mt = &data[i];
        }

        if (!P_IsDoomnumAllowed(mt->type))
            continue;
        if (P_WillSpawnMapThing(mt))
            spawnableThings++;
    }

    if (is_huffman)
        P_HuffFinishOrError(&huff, "P_LoadThings", lump);

    if (spawnableThings <= 0)
        spawnableThings = 1;

    return spawnableThings;
}

/*
 * P_LoadThings
 * Spawns things using the pre-allocated thing pool (pool must be allocated before calling).
 */

static void P_LoadThings (int lump)
{
    int  i;
    int lump_size;
    int numthings;
    const mapthing_t *data = NULL;
    boolean is_huffman = false;
    huff_lump_reader_t huff;

    if (P_HuffInitLumpReader(lump, &huff))
    {
        is_huffman = true;
        lump_size = (int)huff.raw_size;
    }
    else
    {
        data = (const mapthing_t*)W_CacheLumpNum(lump);
        lump_size = W_LumpLength(lump);
    }

    if (lump_size % (int)sizeof(mapthing_t) != 0)
        I_Error("P_LoadThings: invalid lump size %d", lump_size);

    numthings = lump_size / (int)sizeof(mapthing_t);

    if (((!is_huffman) && (!data)) || (!numthings))
        I_Error("P_LoadThings: no things in level");

    if (is_huffman)
        P_HuffResetReader(&huff);

    for (i=0; i<numthings; i++)
    {
        mapthing_t decoded_thing;
        const mapthing_t* mt;

        if (is_huffman)
        {
            P_HuffReadBytesOrError(&huff, &decoded_thing, sizeof(decoded_thing), "P_LoadThings", lump);
            mt = &decoded_thing;
        }
        else
        {
            mt = &data[i];
        }

        if (!P_IsDoomnumAllowed(mt->type))
            continue;

        // Do spawn all other stuff.
        P_SpawnMapThing(mt);
    }

    if (is_huffman)
        P_HuffFinishOrError(&huff, "P_LoadThings", lump);
}

//
// P_LoadLineDefs
// Also counts secret lines for intermissions.
//        ^^^
// ??? killough ???
// Does this mean secrets used to be linedef-based, rather than sector-based?
//
// killough 4/4/98: split into two functions, to allow sidedef overloading
//
// killough 5/3/98: reformatted, cleaned up

static void P_LoadLineDefs (int lump)
{
    _g->numlines = W_LumpLength (lump) / sizeof(line_t);
    _g->lines = W_CacheLumpNum (lump);

    _g->linedata = Z_Calloc(_g->numlines,sizeof(linedata_t),PU_LEVEL,0);
    _g->line_special_cleared =
        Z_Calloc((_g->numlines + 7) / 8, sizeof(*_g->line_special_cleared), PU_LEVEL, 0);
    _g->line_special_stairdir_toggled =
        Z_Calloc((_g->numlines + 7) / 8, sizeof(*_g->line_special_stairdir_toggled), PU_LEVEL, 0);
}

// killough 4/4/98: delay using sidedefs until they are loaded
// killough 5/3/98: reformatted, cleaned up

static void P_LoadLineDefs2(int lump)
{
    /*
  int i = _g->numlines;
  register line_t *ld = _g->lines;
  for (;i--;ld++)
    {
      ld->frontsector = _g->sides[ld->sidenum[0]].sector; //e6y: Can't be NO_INDEX here
      ld->backsector  = ld->sidenum[1]!=NO_INDEX ? _g->sides[ld->sidenum[1]].sector : 0;
    }
    */
}

typedef enum
{
    SIDEDEF_FORMAT_VANILLA = 0,
    SIDEDEF_FORMAT_COMPACT,
    SIDEDEF_FORMAT_VAR
} sidedef_load_format_t;

static sidedef_load_format_t g_sidedef_load_format = SIDEDEF_FORMAT_COMPACT;

//
// P_LoadSideDefs
//
// killough 4/4/98: split into two functions

static void P_LoadSideDefs (int lump)
{
  int lump_size;
  const byte *data = NULL;
  boolean is_huffman = false;
  huff_lump_reader_t huff;
  boolean can_be_vanilla;
  boolean can_be_compact;
  boolean is_vanilla = false;
  const char *format_name = "COMPACT";

  g_sidedef_load_format = SIDEDEF_FORMAT_COMPACT;

  if (P_HuffInitLumpReader(lump, &huff))
  {
    is_huffman = true;
    lump_size = (int)huff.raw_size;
  }
  else
  {
    data = W_CacheLumpNum(lump);
    lump_size = W_LumpLength(lump);
  }

  if (lump_size >= (int)GBADOOM_SIDEDEF_VAR_HEADER_SIZE)
  {
    byte header[GBADOOM_SIDEDEF_VAR_HEADER_SIZE];
    unsigned int var_count;

    if (is_huffman)
    {
      P_HuffResetReader(&huff);
      P_HuffReadBytesOrError(&huff, header, sizeof(header), "P_LoadSideDefs", lump);
    }
    else
    {
      memcpy(header, data, sizeof(header));
    }

    if (P_ReadU32LE(header) == GBADOOM_SIDEDEF_VAR_MAGIC)
    {
      var_count = P_ReadU16LE(header + 4);
      if (var_count == 0)
        I_Error("P_LoadSideDefs: no sidedefs in var lump");

      _g->numsides = (int)var_count;
      g_sidedef_load_format = SIDEDEF_FORMAT_VAR;
      lprintf(LO_INFO, "P_LoadSideDefs: format=VAR numsides=%d\n", _g->numsides);
      _g->sides = Z_Calloc(_g->numsides, sizeof(side_t), PU_LEVEL, 0);
      return;
    }
  }

  can_be_vanilla = (lump_size % sizeof(mapvanillasidedef_t) == 0);
  can_be_compact = (lump_size % sizeof(mapsidedef_t) == 0);

  if (can_be_vanilla && !can_be_compact)
  {
    is_vanilla = true;
  }
  else if (!can_be_vanilla && can_be_compact)
  {
    is_vanilla = false;
  }
  else if (can_be_vanilla && can_be_compact)
  {
    // Ambiguous (like vanilla SIDEDEFS lump size divisible by both 30 and 12).
    // Choose the interpretation with fewer out-of-range sector references.
    int i;
    int vanilla_count = lump_size / sizeof(mapvanillasidedef_t);
    int compact_count = lump_size / sizeof(mapsidedef_t);
    int vanilla_bad = 0;
    int compact_bad = 0;

    if (is_huffman)
    {
      P_HuffResetReader(&huff);
      for (i = 0; i < vanilla_count; i++)
      {
        mapvanillasidedef_t msd;
        unsigned short sector_num;
        P_HuffReadBytesOrError(&huff, &msd, sizeof(msd), "P_LoadSideDefs", lump);
        sector_num = (unsigned short)SHORT(msd.sector);
        if (sector_num >= _g->numsectors)
          vanilla_bad++;
      }
      P_HuffFinishOrError(&huff, "P_LoadSideDefs", lump);

      P_HuffResetReader(&huff);
      for (i = 0; i < compact_count; i++)
      {
        mapsidedef_t msd;
        unsigned short sector_num;
        P_HuffReadBytesOrError(&huff, &msd, sizeof(msd), "P_LoadSideDefs", lump);
        sector_num = (unsigned short)SHORT(msd.sector);
        if (sector_num >= _g->numsectors)
          compact_bad++;
      }
      P_HuffFinishOrError(&huff, "P_LoadSideDefs", lump);
    }
    else
    {
      for (i = 0; i < vanilla_count; i++)
      {
        const mapvanillasidedef_t *msd = (const mapvanillasidedef_t *)data + i;
        unsigned short sector_num = (unsigned short)SHORT(msd->sector);
        if (sector_num >= _g->numsectors)
          vanilla_bad++;
      }

      for (i = 0; i < compact_count; i++)
      {
        const mapsidedef_t *msd = (const mapsidedef_t *)data + i;
        unsigned short sector_num = (unsigned short)SHORT(msd->sector);
        if (sector_num >= _g->numsectors)
          compact_bad++;
      }
    }

#if defined(NUMWORKS)
    if (compact_bad == vanilla_bad)
      is_vanilla = false;
    else
      is_vanilla = (vanilla_bad < compact_bad);
#else
    is_vanilla = (vanilla_bad <= compact_bad);
#endif

    lprintf(LO_WARN,
        "P_LoadSideDefs: ambiguous sidedef lump (%d bytes), chose %s (vanilla bad=%d compact bad=%d)\n",
        lump_size,
        is_vanilla ? "VANILLA" : "COMPACT",
        vanilla_bad,
        compact_bad);
  }
  else
  {
    I_Error("P_LoadSideDefs: invalid sidedef lump size %d", lump_size);
  }

  if (is_vanilla)
  {
    _g->numsides = lump_size / sizeof(mapvanillasidedef_t);
    g_sidedef_load_format = SIDEDEF_FORMAT_VANILLA;
    format_name = "VANILLA";
  }
  else
  {
    _g->numsides = lump_size / sizeof(mapsidedef_t);
    g_sidedef_load_format = SIDEDEF_FORMAT_COMPACT;
    format_name = "COMPACT";
  }

  lprintf(LO_INFO, "P_LoadSideDefs: format=%s numsides=%d\n",
      format_name, _g->numsides);

  _g->sides = Z_Calloc(_g->numsides,sizeof(side_t),PU_LEVEL,0);
}

// killough 4/4/98: delay using texture names until
// after linedefs are loaded, to allow overloading.
// killough 5/3/98: reformatted, cleaned up

static void P_LoadSideDefs2(int lump)
{
    const byte *data = NULL;
    int lump_size;
    int  i;
    boolean is_huffman = false;
    huff_lump_reader_t huff;

    if (P_HuffInitLumpReader(lump, &huff))
    {
        is_huffman = true;
        lump_size = (int)huff.raw_size;
    }
    else
    {
        data = W_CacheLumpNum(lump);
        lump_size = W_LumpLength(lump);
    }

    if (g_sidedef_load_format == SIDEDEF_FORMAT_VAR)
    {
        const byte *cursor = NULL;
        const byte *end = NULL;

        if (is_huffman)
        {
            byte header[GBADOOM_SIDEDEF_VAR_HEADER_SIZE];
            P_HuffReadBytesOrError(&huff, header, sizeof(header), "P_LoadSideDefs2", lump);
            if (P_ReadU32LE(header) != GBADOOM_SIDEDEF_VAR_MAGIC
                || P_ReadU16LE(header + 4) != (unsigned int)_g->numsides)
            {
                I_Error("P_LoadSideDefs2: invalid var sidedef header in lump %d", lump);
            }
        }
        else
        {
            if (lump_size < (int)GBADOOM_SIDEDEF_VAR_HEADER_SIZE)
                I_Error("P_LoadSideDefs2: truncated var sidedef lump %d", lump);

            if (P_ReadU32LE(data) != GBADOOM_SIDEDEF_VAR_MAGIC
                || P_ReadU16LE(data + 4) != (unsigned int)_g->numsides)
            {
                I_Error("P_LoadSideDefs2: invalid var sidedef header in lump %d", lump);
            }

            cursor = data + GBADOOM_SIDEDEF_VAR_HEADER_SIZE;
            end = data + lump_size;
        }

        for (i=0; i<_g->numsides; i++)
        {
            byte desc0;
            byte desc1;
            int textureoffset = 0;
            int rowoffset = 0;
            unsigned int toptexture = 0;
            unsigned int bottomtexture = 0;
            unsigned int midtexture = 0;
            unsigned int sector_num;
            register side_t *sd = _g->sides + i;
            register sector_t *sec = NULL;

            desc0 = P_ReadPackedByteOrError(is_huffman, &huff, &cursor, end, "P_LoadSideDefs2", lump);
            desc1 = P_ReadPackedByteOrError(is_huffman, &huff, &cursor, end, "P_LoadSideDefs2", lump);

            if (desc0 & 0x01)
            {
                if (desc0 & 0x02)
                    textureoffset =
                        P_ReadPackedS16OrError(is_huffman, &huff, &cursor, end, "P_LoadSideDefs2", lump);
                else
                    textureoffset =
                        (int)(signed char)P_ReadPackedByteOrError(is_huffman, &huff, &cursor, end, "P_LoadSideDefs2", lump);
            }

            if (desc0 & 0x04)
            {
                if (desc0 & 0x08)
                    rowoffset =
                        P_ReadPackedS16OrError(is_huffman, &huff, &cursor, end, "P_LoadSideDefs2", lump);
                else
                    rowoffset =
                        (int)(signed char)P_ReadPackedByteOrError(is_huffman, &huff, &cursor, end, "P_LoadSideDefs2", lump);
            }

            if (desc0 & 0x10)
            {
                if (desc0 & 0x20)
                    toptexture =
                        P_ReadPackedU16OrError(is_huffman, &huff, &cursor, end, "P_LoadSideDefs2", lump);
                else
                    toptexture =
                        (unsigned int)P_ReadPackedByteOrError(is_huffman, &huff, &cursor, end, "P_LoadSideDefs2", lump);
            }

            if (desc0 & 0x40)
            {
                if (desc0 & 0x80)
                    bottomtexture =
                        P_ReadPackedU16OrError(is_huffman, &huff, &cursor, end, "P_LoadSideDefs2", lump);
                else
                    bottomtexture =
                        (unsigned int)P_ReadPackedByteOrError(is_huffman, &huff, &cursor, end, "P_LoadSideDefs2", lump);
            }

            if (desc1 & 0x01)
            {
                if (desc1 & 0x02)
                    midtexture =
                        P_ReadPackedU16OrError(is_huffman, &huff, &cursor, end, "P_LoadSideDefs2", lump);
                else
                    midtexture =
                        (unsigned int)P_ReadPackedByteOrError(is_huffman, &huff, &cursor, end, "P_LoadSideDefs2", lump);
            }

            if (desc1 & 0x04)
            {
                sector_num =
                    P_ReadPackedU16OrError(is_huffman, &huff, &cursor, end, "P_LoadSideDefs2", lump);
            }
            else
            {
                sector_num =
                    (unsigned int)P_ReadPackedByteOrError(is_huffman, &huff, &cursor, end, "P_LoadSideDefs2", lump);
            }

            { /* cph 2006/09/30 - catch out-of-range sector numbers; use sector 0 instead */
                if (sector_num >= _g->numsectors)
                {
                    lprintf(LO_WARN,"P_LoadSideDefs2: sidedef %i has out-of-range sector num %u\n", i, sector_num);
                    sector_num = 0;
                }
                sd->sector = sec = &_g->sectors[sector_num];
            }

            sd->textureoffset = (short)textureoffset;
            sd->rowoffset = (short)rowoffset;
            sd->midtexture = (unsigned short)midtexture;
            sd->toptexture = (unsigned short)toptexture;
            sd->bottomtexture = (unsigned short)bottomtexture;
        }

        if (!is_huffman && cursor != end)
            I_Error("P_LoadSideDefs2: trailing data in var sidedef lump %d", lump);
    }
    else if (g_sidedef_load_format == SIDEDEF_FORMAT_VANILLA)
    {
        // VANILLA format: texture names as char[8], need to convert to indices
        for (i=0; i<_g->numsides; i++)
        {
            mapvanillasidedef_t decoded_sidedef;
            register const mapvanillasidedef_t *msd;
            register side_t *sd = _g->sides + i;
            register sector_t *sec;

            if (is_huffman)
            {
                P_HuffReadBytesOrError(&huff, &decoded_sidedef, sizeof(decoded_sidedef), "P_LoadSideDefs2", lump);
                msd = &decoded_sidedef;
            }
            else
            {
                msd = (const mapvanillasidedef_t *) data + i;
            }

            sd->textureoffset = msd->textureoffset;
            sd->rowoffset = msd->rowoffset;

            { /* cph 2006/09/30 - catch out-of-range sector numbers; use sector 0 instead */
                unsigned short sector_num = SHORT(msd->sector);
                if (sector_num >= _g->numsectors)
                {
                    lprintf(LO_WARN,"P_LoadSideDefs2: sidedef %i has out-of-range sector num %u\n", i, sector_num);
                    sector_num = 0;
                }
                sd->sector = sec = &_g->sectors[sector_num];
            }

            // Convert texture names to indices
            sd->midtexture = R_LoadTextureByName(msd->midtexture);
            sd->toptexture = R_LoadTextureByName(msd->toptexture);
            sd->bottomtexture = R_LoadTextureByName(msd->bottomtexture);
        }
    }
    else
    {
        // COMPACT format: texture indices as short, use directly
        for (i=0; i<_g->numsides; i++)
        {
            mapsidedef_t decoded_sidedef;
            register const mapsidedef_t *msd;
            register side_t *sd = _g->sides + i;
            register sector_t *sec;

            if (is_huffman)
            {
                P_HuffReadBytesOrError(&huff, &decoded_sidedef, sizeof(decoded_sidedef), "P_LoadSideDefs2", lump);
                msd = &decoded_sidedef;
            }
            else
            {
                msd = (const mapsidedef_t *) data + i;
            }

            sd->textureoffset = msd->textureoffset;
            sd->rowoffset = msd->rowoffset;

            { /* cph 2006/09/30 - catch out-of-range sector numbers; use sector 0 instead */
                unsigned short sector_num = SHORT(msd->sector);
                if (sector_num >= _g->numsectors)
                {
                    lprintf(LO_WARN,"P_LoadSideDefs2: sidedef %i has out-of-range sector num %u\n", i, sector_num);
                    sector_num = 0;
                }
                sd->sector = sec = &_g->sectors[sector_num];
            }

            sd->midtexture = msd->midtexture;
            sd->toptexture = msd->toptexture;
            sd->bottomtexture = msd->bottomtexture;
        }
    }

    if (is_huffman)
        P_HuffFinishOrError(&huff, "P_LoadSideDefs2", lump);
}

//
// jff 10/6/98
// New code added to speed up calculation of internal blockmap
// Algorithm is order of nlines*(ncols+nrows) not nlines*ncols*nrows
//

#define blkshift 7               /* places to shift rel position for cell num */
#define blkmask ((1<<blkshift)-1)/* mask for rel position within cell */
#define blkmargin 0              /* size guardband around map used */
                                 // jff 10/8/98 use guardband>0
                                 // jff 10/12/98 0 ok with + 1 in rows,cols

typedef struct linelist_t        // type used to list lines in each block
{
  long num;
  struct linelist_t *next;
} linelist_t;

//
// P_LoadBlockMap
//
// killough 3/1/98: substantially modified to work
// towards removing blockmap limit (a wad limitation)
//
// killough 3/30/98: Rewritten to remove blockmap limit,
// though current algorithm is brute-force and unoptimal.
//

static void P_LoadBlockMap (int lump)
{
    int i;
    int leading_zero_blocks = 0;
    int valid_blocks = 0;
    int blockmap_short_count;
    int lump_size_bytes;
    const byte *raw_blockmap;

    raw_blockmap = W_CacheLumpNum(lump);
    lump_size_bytes = W_LumpLength(lump);

    _g->blockmaplump = (const short *)raw_blockmap;
    blockmap_short_count = lump_size_bytes / (int)sizeof(short);

    _g->blockmap_is_compact = false;
    _g->blockmap_compact_data = NULL;
    _g->blockmap_compact_rowofs = NULL;
    _g->blockmap_compact_lists = NULL;
    _g->blockmap_compact_end = NULL;
    _g->blockmap_compact_bitmap_bytes = 0;

    if (lump_size_bytes >= (int)GBADOOM_BLOCKMAP_COMPACT_HEADER_SIZE
        && P_ReadU32LE(raw_blockmap) == GBADOOM_BLOCKMAP_COMPACT_MAGIC)
    {
        int orgx = (short)P_ReadU16LE(raw_blockmap + 4);
        int orgy = (short)P_ReadU16LE(raw_blockmap + 6);
        unsigned int width = P_ReadU16LE(raw_blockmap + 8);
        unsigned int height = P_ReadU16LE(raw_blockmap + 10);
        unsigned int list_base = P_ReadU16LE(raw_blockmap + 12);
        unsigned int bitmap_bytes;
        unsigned int row_table_bytes;
        const byte *rowofs;
        const byte *lump_end = raw_blockmap + lump_size_bytes;

        if (width == 0 || height == 0)
            I_Error("P_LoadBlockMap: invalid compact dimensions %ux%u", width, height);

        bitmap_bytes = (width + 7u) >> 3;
        row_table_bytes = height * 2u;
        if (GBADOOM_BLOCKMAP_COMPACT_HEADER_SIZE + row_table_bytes > (unsigned int)lump_size_bytes)
            I_Error("P_LoadBlockMap: compact row table overrun");
        if (list_base < GBADOOM_BLOCKMAP_COMPACT_HEADER_SIZE + row_table_bytes
            || list_base > (unsigned int)lump_size_bytes)
            I_Error("P_LoadBlockMap: invalid compact list base (%u/%d)", list_base, lump_size_bytes);

        rowofs = raw_blockmap + GBADOOM_BLOCKMAP_COMPACT_HEADER_SIZE;
        for (i = 0; i < (int)height; i++)
        {
            unsigned int row_offset = P_ReadU16LE(rowofs + (i * 2));
            const byte *row;
            unsigned int desc_count;
            const byte *row_end;

            if (row_offset < GBADOOM_BLOCKMAP_COMPACT_HEADER_SIZE + row_table_bytes
                || row_offset >= list_base)
            {
                I_Error("P_LoadBlockMap: invalid compact row offset %u", row_offset);
            }

            row = raw_blockmap + row_offset;
            if (row + 2 > raw_blockmap + list_base)
                I_Error("P_LoadBlockMap: truncated compact row header");

            desc_count = P_ReadU16LE(row);
            row_end = row + 2 + bitmap_bytes + (desc_count * 2u);
            if (row_end > raw_blockmap + list_base)
                I_Error("P_LoadBlockMap: compact row data overflow");
        }

        _g->bmaporgx = orgx << FRACBITS;
        _g->bmaporgy = orgy << FRACBITS;
        _g->bmapwidth = (int)width;
        _g->bmapheight = (int)height;
        _g->blockmap = NULL;
        _g->blockmap_has_leading_zero = false;
        _g->blockmap_is_compact = true;
        _g->blockmap_compact_data = raw_blockmap;
        _g->blockmap_compact_rowofs = rowofs;
        _g->blockmap_compact_lists = raw_blockmap + list_base;
        _g->blockmap_compact_end = lump_end;
        _g->blockmap_compact_bitmap_bytes = (unsigned short)bitmap_bytes;

        lprintf(LO_INFO,
            "P_LoadBlockMap: compact rows enabled (%dx%d, bitmap=%u bytes)\n",
            _g->bmapwidth,
            _g->bmapheight,
            (unsigned int)_g->blockmap_compact_bitmap_bytes);
    }
    else
    {
        _g->bmaporgx = _g->blockmaplump[0]<<FRACBITS;
        _g->bmaporgy = _g->blockmaplump[1]<<FRACBITS;
        _g->bmapwidth = _g->blockmaplump[2];
        _g->bmapheight = _g->blockmaplump[3];
        _g->blockmap = _g->blockmaplump+4;
    }

#if defined(NUMWORKS) && PLATFORM_DEVICE
    // Compact mode: keep one global block chain head and filter by block on iteration.
    _g->blocklinks = Z_Calloc(1, sizeof(*_g->blocklinks), PU_LEVEL, 0);
    lprintf(LO_INFO, "P_LoadBlockMap: compact thing links enabled (saved %d bytes)\n",
        (int)((_g->bmapwidth * _g->bmapheight - 1) * (int)sizeof(*_g->blocklinks)));
#else
    // clear out mobj chains - CPhipps - use calloc
    _g->blocklinks = Z_Calloc (_g->bmapwidth*_g->bmapheight,sizeof(*_g->blocklinks),PU_LEVEL,0);
#endif

    if (_g->blockmap_is_compact)
        return;

    // Some generated WADs omit the legacy leading 0 delimiter in each block list.
    // Detect which representation this lump uses so iterators can parse lists correctly.
    for (i = 0; i < _g->bmapwidth * _g->bmapheight; i++)
    {
        int offset = _g->blockmap[i];

        if (offset < 0 || offset >= blockmap_short_count)
            continue;

        valid_blocks++;
        if (_g->blockmaplump[offset] == 0)
            leading_zero_blocks++;
    }

    _g->blockmap_has_leading_zero =
        (valid_blocks > 0) && (leading_zero_blocks == valid_blocks);

    lprintf(LO_INFO,
        "P_LoadBlockMap: leading_zero=%s (%d/%d blocks)\n",
        _g->blockmap_has_leading_zero ? "yes" : "no",
        leading_zero_blocks,
        valid_blocks);
}

//
// P_LoadReject - load the reject table, padding it if it is too short
// totallines must be the number returned by P_GroupLines()
// an underflow will be padded with zeroes, or a doom.exe z_zone header
// 
// this function incorporates e6y's RejectOverrunAddInt code:
// e6y: REJECT overrun emulation code
// It's emulated successfully if the size of overflow no more than 16 bytes.
// No more desync on teeth-32.wad\teeth-32.lmp.
// http://www.doomworld.com/vb/showthread.php?s=&threadid=35214

static void P_LoadReject(int lumpnum)
{
  _g->rejectlump = lumpnum + ML_REJECT;
  _g->rejectmatrix = W_CacheLumpNum(_g->rejectlump);
}

//
// P_GroupLines
// Builds sector line lists and subsector sector numbers.
// Finds block bounding boxes for sectors.
//
// killough 5/3/98: reformatted, cleaned up
// cph 18/8/99: rewritten to avoid O(numlines * numsectors) section
// It makes things more complicated, but saves seconds on big levels
// figgi 09/18/00 -- adapted for gl-nodes

// cph - convenient sub-function
static void P_AddLineToSector(unsigned int line_index, const line_t* li, sector_t* sector)
{
#if defined(NUMWORKS) && PLATFORM_DEVICE
  if (line_index >= (unsigned int)_g->numlines)
    I_Error("P_AddLineToSector: invalid line index (%u/%d)", line_index, _g->numlines);
  if (line_index > 0xFFFFu)
    I_Error("P_AddLineToSector: line index overflow (%u)", line_index);
  sector->lines[sector->linecount++] = (unsigned short)line_index;
#else
  (void)line_index;
  sector->lines[sector->linecount++] = li;
#endif
}

// modified to return totallines (needed by P_LoadReject)
static int P_GroupLines (void)
{
    register const line_t *li;
    register sector_t *sector;
    int i,j, total = _g->numlines;

    // figgi
    for (i=0 ; i<_g->numsubsectors ; i++)
    {
        const seg_t *seg = &_g->segs[_g->subsectors[i].firstline];
        _g->subsectors[i].sector = NULL;
        for(j=0; j<_g->subsectors[i].numlines; j++)
        {
            if(seg->sidenum != NO_INDEX)
            {
                _g->subsectors[i].sector = _g->sides[seg->sidenum].sector;
                break;
            }
            seg++;
        }
        if(_g->subsectors[i].sector == NULL)
            I_Error("P_GroupLines: Subsector a part of no sector!\n");
    }

    // count number of lines in each sector
    for (i=0,li=_g->lines; i<_g->numlines; i++, li++)
    {
        LN_FRONTSECTOR(li)->linecount++;
        if (LN_BACKSECTOR(li) && LN_BACKSECTOR(li) != LN_FRONTSECTOR(li))
        {
            LN_BACKSECTOR(li)->linecount++;
            total++;
        }
    }

    {  // allocate line tables for each sector
#if defined(NUMWORKS) && PLATFORM_DEVICE
        unsigned short *linebuffer = Z_Malloc(total * sizeof(*linebuffer), PU_LEVEL, 0);
#else
        const line_t **linebuffer = Z_Malloc(total * sizeof(*linebuffer), PU_LEVEL, 0);
#endif

        // e6y: REJECT overrun emulation code
        // moved to P_LoadReject

        for (i=0, sector = _g->sectors; i<_g->numsectors; i++, sector++)
        {
            sector->lines = linebuffer;
            linebuffer += sector->linecount;
            sector->linecount = 0;
        }
    }

    // Enter those lines
    for (i=0,li=_g->lines; i<_g->numlines; i++, li++)
    {
        P_AddLineToSector((unsigned int)i, li, LN_FRONTSECTOR(li));
        if (LN_BACKSECTOR(li) && LN_BACKSECTOR(li) != LN_FRONTSECTOR(li))
            P_AddLineToSector((unsigned int)i, li, LN_BACKSECTOR(li));
    }

    for (i=0, sector = _g->sectors; i<_g->numsectors; i++, sector++)
    {
        fixed_t bbox[4];
        M_ClearBox(bbox);

        for(int l = 0; l < sector->linecount; l++)
        {
            const line_t* sl = SECTOR_LINE(sector, l);
            M_AddToBox (bbox, LN_V1(sl)->x, LN_V1(sl)->y);
            M_AddToBox (bbox, LN_V2(sl)->x, LN_V2(sl)->y);
        }


    }

    return total; // this value is needed by the reject overrun emulation code
}


void P_FreeLevelData()
{
    R_ResetPlanes();

    Z_FreeTags(PU_LEVEL, PU_PURGELEVEL-1);

    Z_Free(_g->braintargets);
    _g->braintargets = NULL;
    _g->numbraintargets_alloc = _g->numbraintargets = 0;
}

//
// P_SetupLevel
//
// killough 5/3/98: reformatted, cleaned up

void P_SetupLevel(int episode, int map, int playermask, skill_t skill)
{
    int   i;
    char  lumpname[9];
    int   lumpnum;

    _g->totallive = _g->totalkills = _g->totalitems = _g->totalsecret = 0;
    _g->wminfo.partime = 180;

    for (i=0; i<MAXPLAYERS; i++)
        _g->player.killcount = _g->player.secretcount = _g->player.itemcount = 0;

    // Initial height of PointOfView will be set by player think.
    _g->player.viewz = 1;

    // Make sure all sounds are stopped before Z_FreeTags.
    S_Start();

    P_FreeLevelData();

    //Load the sky texture.
    R_GetTexture(_g->skytexture);

    if (_g->rejectlump != -1)
    { // cph - unlock the reject table
        _g->rejectlump = -1;
    }

    P_InitThinkers();

    // if working with a devlopment map, reload it
    //    W_Reload ();     killough 1/31/98: W_Reload obsolete

    // find map name
    if (_g->gamemode == commercial)
    {
        sprintf(lumpname, "MAP%02d", map);           // killough 1/24/98: simplify
    }
    else
    {
        sprintf(lumpname, "E%dM%d", episode, map);   // killough 1/24/98: simplify
    }

    lumpnum = W_GetNumForName(lumpname);

    // Allocate thing pool as small chunks FIRST, before any other level data,
    // to avoid large contiguous allocations that fail on fragmented heap.
    int thingPoolSize = P_CountThings(lumpnum + ML_THINGS);
    int chunks = (thingPoolSize + THINGS_PER_CHUNK - 1) / THINGS_PER_CHUNK;
    if (chunks > MAX_THING_CHUNKS)
        chunks = MAX_THING_CHUNKS;
    _g->thingChunkCount = chunks;
    for (int c = 0; c < chunks; c++)
    {
        _g->thingChunks[c] = Z_Calloc(THINGS_PER_CHUNK, sizeof(mobj_t), PU_LEVEL, NULL);
        for (int j = 0; j < THINGS_PER_CHUNK; j++)
            _g->thingChunks[c][j].type = MT_NOTHING;
    }
    lprintf(LO_INFO, "P_SetupLevel: thing pool %d (%d chunks)", thingPoolSize, chunks);

    _g->leveltime = 0; _g->totallive = 0;

    P_LoadVertexes  (lumpnum+ML_VERTEXES);
    P_LoadSectors   (lumpnum+ML_SECTORS);
    P_LoadSideDefs  (lumpnum+ML_SIDEDEFS);
    P_LoadLineDefs  (lumpnum+ML_LINEDEFS);
    P_LoadSideDefs2 (lumpnum+ML_SIDEDEFS);
    P_LoadLineDefs2 (lumpnum+ML_LINEDEFS);
    P_LoadBlockMap  (lumpnum+ML_BLOCKMAP);


    P_LoadSubsectors(lumpnum + ML_SSECTORS);
    P_LoadNodes(lumpnum + ML_NODES);
    P_LoadSegs(lumpnum + ML_SEGS);

    P_GroupLines();

    // reject loading and underflow padding separated out into new function
    // P_GroupLines modified to return a number the underflow padding needs
    P_LoadReject(lumpnum);

    // Note: you don't need to clear player queue slots --
    // a much simpler fix is in g_game.c -- killough 10/98

    /* cph - reset all multiplayer starts */
    memset(_g->playerstarts,0,sizeof(_g->playerstarts));

    for (i = 0; i < MAXPLAYERS; i++)
        _g->player.mo = NULL;

    P_MapStart();

    P_LoadThings(lumpnum+ML_THINGS);

    {
        if (_g->playeringame && !_g->player.mo)
            I_Error("P_SetupLevel: missing player %d start\n", i+1);
    }

    // killough 3/26/98: Spawn icon landings:
    if (_g->gamemode==commercial)
        P_SpawnBrainTargets();

    // set up world state
    P_SpawnSpecials();

    P_MapEnd();

}

//
// P_Init
//
void P_Init (void)
{
    lprintf(LO_INFO, "P_InitSwitchList");
    P_InitSwitchList();

    lprintf(LO_INFO, "P_InitPicAnims");
    P_InitPicAnims();

    lprintf(LO_INFO, "R_InitSprites");
    R_InitSprites(sprnames);
}
