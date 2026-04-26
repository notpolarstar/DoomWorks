// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//	Zone Memory Allocation. Neat.
//
//-----------------------------------------------------------------------------

#include "z_zone.h"
#include "doomdef.h"
#include "doomtype.h"
#include "lprintf.h"
#if defined(NUMWORKS) && PLATFORM_DEVICE
#include "i_system_e32.h"
#define NUMWORKS_CHECKPOINT(msg) I_DebugCheckpoint_e32(msg)
#else
#define NUMWORKS_CHECKPOINT(msg) do { } while (0)
#endif
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#if defined(NUMWORKS) && PLATFORM_DEVICE
#define USE_SYSTEM_ALLOCATOR 1
#include "storage.h"
#else
#define USE_SYSTEM_ALLOCATOR 0
#endif

#if USE_SYSTEM_ALLOCATOR

#define SYSALLOC_MAGIC 0x414Cu

typedef struct sysallocblock_s
{
    uint32_t size;
    uint16_t tag;
    uint16_t magic;
    void** user;
    struct sysallocblock_s* next;
} sysallocblock_t;

static sysallocblock_t* s_sysalloc_head = NULL;
static int running_count = 0;

#if defined(NUMWORKS) && PLATFORM_DEVICE

typedef struct fsallocblock_s
{
    uint32_t size;
    uint16_t tag;
    void** user;
    void* ptr;
    uint8_t in_use;
} fsallocblock_t;

static fsallocblock_t s_fsalloc_blocks[FSALLOC_MAX_BLOCKS];
static uint8_t* s_fsalloc_pool_base = NULL;
static size_t s_fsalloc_pool_size = 0;

static fsallocblock_t* Z_FsAlloc_FindByPtr(void* ptr)
{
    int i;

    for (i = 0; i < FSALLOC_MAX_BLOCKS; ++i)
    {
        if (s_fsalloc_blocks[i].in_use && s_fsalloc_blocks[i].ptr == ptr)
            return &s_fsalloc_blocks[i];
    }

    return NULL;
}

static fsallocblock_t* Z_FsAlloc_ReserveSlot(void)
{
    int i;

    for (i = 0; i < FSALLOC_MAX_BLOCKS; ++i)
    {
        if (!s_fsalloc_blocks[i].in_use)
            return &s_fsalloc_blocks[i];
    }

    return NULL;
}

static void* Z_FsAlloc_CreateRecord(const char* name, size_t len)
{
    char* record_start;
    char* content_start;
    char* record_end;
    char* storage_end;
    size_t name_len;
    size_t total_size;
    uint16_t total_size_u16;

    record_start = (char*)extapp_nextFree();
    if (record_start == NULL)
        return NULL;

    storage_end = (char*)extapp_address() + extapp_size();
    name_len = strlen(name) + 1;
    total_size = 2u + name_len + len;

    if (total_size > 0xFFFFu)
        return NULL;

    if (record_start + total_size > storage_end)
        return NULL;

    total_size_u16 = (uint16_t)total_size;
    memcpy(record_start, &total_size_u16, sizeof(total_size_u16));
    memcpy(record_start + 2, name, name_len);

    content_start = record_start + 2 + name_len;
    if (len > 0)
        memset(content_start, 0, len);

    record_end = record_start + total_size;
    if (record_end + 1 < storage_end)
    {
        // Keep storage record list terminated without clearing the whole flash tail.
        record_end[0] = 0;
        record_end[1] = 0;
    }

    return content_start;
}

static int Z_FsAlloc_CompareByPtr(const void* a, const void* b)
{
    const fsallocblock_t* const* lhs = (const fsallocblock_t* const*)a;
    const fsallocblock_t* const* rhs = (const fsallocblock_t* const*)b;

    if ((*lhs)->ptr < (*rhs)->ptr)
        return -1;
    if ((*lhs)->ptr > (*rhs)->ptr)
        return 1;
    return 0;
}

static int Z_FsPool_EnsureInitialized(void)
{
    const char* existing_content;
    size_t existing_len;
    const size_t name_len = strlen(FSALLOC_POOL_FILE) + 1;
    size_t max_len;
    size_t align_offset;
    uint8_t* record_start;
    uint8_t* usable_start;
    uint8_t* storage_end;

    if (s_fsalloc_pool_base != NULL && s_fsalloc_pool_size > 0)
        return 1;

    NUMWORKS_CHECKPOINT("Z_FS_POOL: ensure init");
    existing_content = extapp_fileRead(FSALLOC_POOL_FILE, &existing_len);
    if (existing_content != NULL && existing_len > 0)
    {
        align_offset = ((uintptr_t)existing_content) & 3u;
        if (align_offset != 0)
            align_offset = 4u - align_offset;

        NUMWORKS_CHECKPOINT("Z_FS_POOL: existing file reused");
        usable_start = (uint8_t*)existing_content + align_offset;
        if (existing_len <= align_offset)
            return 0;

        s_fsalloc_pool_base = usable_start;
        s_fsalloc_pool_size = existing_len - align_offset;
        NUMWORKS_CHECKPOINT("Z_FS_POOL: aligned existing pool");
        return 1;
    }

    NUMWORKS_CHECKPOINT("Z_FS_POOL: creating backing file");
    record_start = (uint8_t*)extapp_nextFree();
    if (record_start == NULL)
        return 0;

    storage_end = (uint8_t*)(extapp_address() + extapp_size());
    if (record_start >= storage_end)
        return 0;

    if ((size_t)(storage_end - record_start) <= (2u + name_len + 4u))
        return 0;

    max_len = (size_t)(storage_end - record_start) - (2u + name_len);
    NUMWORKS_CHECKPOINT("Z_FS_POOL: write record");
    usable_start = (uint8_t*)Z_FsAlloc_CreateRecord(FSALLOC_POOL_FILE, max_len);
    if (usable_start == NULL)
    {
        NUMWORKS_CHECKPOINT("Z_FS_POOL: create record failed");
        return 0;
    }

    align_offset = ((uintptr_t)usable_start) & 3u;
    if (align_offset != 0)
        align_offset = 4u - align_offset;

    if (max_len <= align_offset)
        return 0;

    s_fsalloc_pool_base = usable_start + align_offset;
    s_fsalloc_pool_size = max_len - align_offset;
    NUMWORKS_CHECKPOINT("Z_FS_POOL: aligned new pool");
    NUMWORKS_CHECKPOINT("Z_FS_POOL: init complete");
    return 1;
}

static void* Z_FsPool_Alloc(size_t aligned_size)
{
    fsallocblock_t* used_blocks[FSALLOC_MAX_BLOCKS];
    int used_count = 0;
    int i;
    uint8_t* cursor;
    uint8_t* pool_end;

    if (!Z_FsPool_EnsureInitialized())
    {
        NUMWORKS_CHECKPOINT("Z_FS_ALLOC: pool init failed");
        return NULL;
    }

    pool_end = s_fsalloc_pool_base + s_fsalloc_pool_size;
    NUMWORKS_CHECKPOINT("Z_FS_ALLOC: scanning slots");

    for (i = 0; i < FSALLOC_MAX_BLOCKS; ++i)
    {
        if (!s_fsalloc_blocks[i].in_use)
            continue;
        if ((uint8_t*)s_fsalloc_blocks[i].ptr < s_fsalloc_pool_base
            || (uint8_t*)s_fsalloc_blocks[i].ptr >= pool_end)
            continue;
        used_blocks[used_count++] = &s_fsalloc_blocks[i];
    }

    qsort(used_blocks, (size_t)used_count, sizeof(used_blocks[0]), Z_FsAlloc_CompareByPtr);

    cursor = s_fsalloc_pool_base;
    for (i = 0; i < used_count; ++i)
    {
        uint8_t* block_start = (uint8_t*)used_blocks[i]->ptr;
        uint8_t* block_end = block_start + used_blocks[i]->size;

        if (block_start > cursor && (size_t)(block_start - cursor) >= aligned_size)
        {
            NUMWORKS_CHECKPOINT("Z_FS_ALLOC: found gap");
            return cursor;
        }

        if (block_end > cursor)
            cursor = block_end;
    }

    if (pool_end > cursor && (size_t)(pool_end - cursor) >= aligned_size)
    {
        NUMWORKS_CHECKPOINT("Z_FS_ALLOC: using tail gap");
        return cursor;
    }

    NUMWORKS_CHECKPOINT("Z_FS_ALLOC: out of space");
    return NULL;
}

static void* Z_TryFsFallback(size_t aligned_size, int tag, void** user)
{
    fsallocblock_t* slot;
    void* ptr;

    if (!I_IsFilesystemEnabled_e32())
        return NULL;

    if (user == NULL && tag >= PU_PURGELEVEL)
        I_Error("Z_Malloc: an owner is required for purgable blocks");

    slot = Z_FsAlloc_ReserveSlot();
    if (slot == NULL)
    {
        NUMWORKS_CHECKPOINT("Z_FS_ALLOC: no free slot");
        return NULL;
    }

    ptr = Z_FsPool_Alloc(aligned_size);
    if (ptr == NULL)
    {
        NUMWORKS_CHECKPOINT("Z_FS_ALLOC: pool alloc failed");
        return NULL;
    }

    slot->size = (uint32_t)aligned_size;
    slot->tag = (uint16_t)tag;
    slot->user = user ? user : (void**)2;
    slot->ptr = ptr;
    slot->in_use = 1;

    if (user)
        *user = ptr;

    NUMWORKS_CHECKPOINT("Z_FS_ALLOC: success");
    running_count += (int)aligned_size;
    printf("Alloc(fs): %d (%d)\n", (int)aligned_size, running_count);

    return ptr;
}
#endif

static size_t Z_BlockSizeForPtr(const void* ptr)
{
#if defined(NUMWORKS) && PLATFORM_DEVICE
    fsallocblock_t* fsblock = Z_FsAlloc_FindByPtr((void*)ptr);
    if (fsblock != NULL)
        return fsblock->size;
#endif

    {
        const sysallocblock_t* block = ((const sysallocblock_t*)ptr) - 1;
        if (block->magic != SYSALLOC_MAGIC)
            I_Error("Z_BlockSizeForPtr(system): bad magic");
        return block->size;
    }
}

static void Z_SysAlloc_Unlink(sysallocblock_t* block)
{
    sysallocblock_t** link = &s_sysalloc_head;

    while (*link != NULL && *link != block)
        link = &(*link)->next;

    if (*link == NULL)
        I_Error("Z_SysAlloc_Unlink: block not found");

    *link = block->next;
}

void Z_Init(void)
{
    NUMWORKS_CHECKPOINT("Z_Init");
    s_sysalloc_head = NULL;
#if defined(NUMWORKS) && PLATFORM_DEVICE
    memset(s_fsalloc_blocks, 0, sizeof(s_fsalloc_blocks));
    s_fsalloc_pool_base = NULL;
    s_fsalloc_pool_size = 0;
#endif
    lprintf(LO_INFO, "Z_Init: using system malloc/free allocator (zone disabled)");
}

unsigned int Z_GetHeapSize(void)
{
    return 0;
}

unsigned int Z_GetFreeMemory(void)
{
    return 0;
}

unsigned int Z_GetAllocatedMemory(void)
{
    return running_count > 0 ? (unsigned int)running_count : 0;
}

void* Z_Malloc(int size, int tag, void** user)
{
    sysallocblock_t* block;
    void* fallback_ptr;
    const size_t aligned_size = (size <= 0) ? 4u : (size_t)((size + 3) & ~3);
    const size_t total_size = sizeof(sysallocblock_t) + aligned_size;

    if (user == NULL && tag >= PU_PURGELEVEL)
        I_Error("Z_Malloc: an owner is required for purgable blocks");

    block = (sysallocblock_t*)malloc(total_size);
    if (block == NULL)
    {
#if defined(NUMWORKS) && PLATFORM_DEVICE
        fallback_ptr = Z_TryFsFallback(aligned_size, tag, user);
        if (fallback_ptr != NULL)
            return fallback_ptr;
#endif
        I_Error("Z_Malloc(system): failed allocation of %i bytes\nUsed: %d bytes", (int)aligned_size, running_count);
    }

    block->size = aligned_size;
    block->tag = (uint16_t)tag;
    block->magic = SYSALLOC_MAGIC;
    block->user = user ? user : (void**)2;
    block->next = s_sysalloc_head;
    s_sysalloc_head = block;

    if (user)
        *user = (void*)(block + 1);

    running_count += (int)aligned_size;
    printf("Alloc(sys): %d (%d)\n", (int)aligned_size, running_count);

    return (void*)(block + 1);
}

void Z_Free(void* ptr)
{
    sysallocblock_t* block;
#if defined(NUMWORKS) && PLATFORM_DEVICE
    fsallocblock_t* fsblock;
#endif

    if (ptr == NULL)
        return;

#if defined(NUMWORKS) && PLATFORM_DEVICE
    fsblock = Z_FsAlloc_FindByPtr(ptr);
    if (fsblock != NULL)
    {
        NUMWORKS_CHECKPOINT("Z_Free: fs block");
        if (fsblock->user > (void**)0x100)
            *fsblock->user = 0;

        running_count -= (int)fsblock->size;
        printf("Free(fs): %d\n", running_count);
        memset(fsblock, 0, sizeof(*fsblock));
        return;
    }
#endif

    block = ((sysallocblock_t*)ptr) - 1;
    if (block->magic != SYSALLOC_MAGIC)
        I_Error("Z_Free(system): bad magic");

    if (block->user > (void**)0x100)
        *block->user = 0;

    Z_SysAlloc_Unlink(block);

    running_count -= (int)block->size;
    printf("Free(sys): %d\n", running_count);

    free(block);
}

void Z_FreeTags(int lowtag, int hightag)
{
    sysallocblock_t* block = s_sysalloc_head;

    while (block != NULL)
    {
        sysallocblock_t* next = block->next;
        if (block->tag >= lowtag && block->tag <= hightag)
            Z_Free((void*)(block + 1));
        block = next;
    }

#if defined(NUMWORKS) && PLATFORM_DEVICE
    {
        int i;
            NUMWORKS_CHECKPOINT("Z_FreeTags: fs scan");
        for (i = 0; i < FSALLOC_MAX_BLOCKS; ++i)
        {
            if (s_fsalloc_blocks[i].in_use
                && s_fsalloc_blocks[i].tag >= lowtag
                && s_fsalloc_blocks[i].tag <= hightag)
            {
                Z_Free(s_fsalloc_blocks[i].ptr);
            }
        }
    }
#endif
}

void Z_CheckHeap(void)
{
    sysallocblock_t* block = s_sysalloc_head;

    while (block != NULL)
    {
        if (block->magic != SYSALLOC_MAGIC)
            I_Error("Z_CheckHeap(system): bad magic");
        block = block->next;
    }

#if defined(NUMWORKS) && PLATFORM_DEVICE
    {
        int i;
        NUMWORKS_CHECKPOINT("Z_CheckHeap: fs scan");
        for (i = 0; i < FSALLOC_MAX_BLOCKS; ++i)
        {
            if (!s_fsalloc_blocks[i].in_use)
                continue;

            if (s_fsalloc_blocks[i].ptr == NULL)
                I_Error("Z_CheckHeap(system): fs block has null ptr");

            if (s_fsalloc_pool_base == NULL || s_fsalloc_pool_size == 0)
                I_Error("Z_CheckHeap(system): fs pool not initialized");

            if ((uint8_t*)s_fsalloc_blocks[i].ptr < s_fsalloc_pool_base
                || ((uint8_t*)s_fsalloc_blocks[i].ptr + s_fsalloc_blocks[i].size) > (s_fsalloc_pool_base + s_fsalloc_pool_size))
            {
                I_Error("Z_CheckHeap(system): fs block out of pool bounds");
            }
        }
    }
#endif
}

void* Z_Calloc(size_t count, size_t size, int tag, void** user)
{
    const size_t bytes = count * size;
    void* ptr = Z_Malloc((int)bytes, tag, user);
    if (ptr != NULL)
        memset(ptr, 0, bytes);
    return ptr;
}

char* Z_Strdup(const char* s)
{
    const size_t len = strlen(s);
    char* ptr;

    if (len == 0)
        return NULL;

    ptr = (char*)Z_Malloc((int)(len + 1), PU_STATIC, NULL);
    if (ptr != NULL)
        strcpy(ptr, s);
    return ptr;
}

void* Z_Realloc(void* ptr, size_t n, int tag, void** user)
{
    void* p = Z_Malloc((int)n, tag, user);
    if (ptr != NULL)
    {
        const size_t old_size = Z_BlockSizeForPtr(ptr);
        const size_t copy_size = (n <= old_size) ? n : old_size;
        memcpy(p, ptr, copy_size);
        Z_Free(ptr);
        if (user)
            *user = p;
    }
    return p;
}

#else


//
// ZONE MEMORY ALLOCATION
//
// There is never any space between memblocks,
//  and there will never be two contiguous free memblocks.
// The rover can be left pointing at a non-empty block.
//
// It is of no value to free a cachable block,
//  because it will get overwritten automatically if needed.
//

#define ZONEID	0x1d4a11

#if defined(NUMWORKS) && PLATFORM_DEVICE
const unsigned int maxHeapSize = (192 * 1024);
const unsigned int minHeapSize = (32 * 1024);
const unsigned int heapStepSize = 512;
#else
const unsigned int maxHeapSize = (256 * 1024);
const unsigned int minHeapSize = (96 * 1024);
const unsigned int heapStepSize = 1024;
#endif

#ifndef GBA
    static int running_count = 0;
#include <stdio.h>
#endif

static unsigned int s_zone_heap_size = 0;

typedef struct memblock_s
{
    unsigned int size:24;	// including the header and possibly tiny fragments
    unsigned int tag:4;	// purgelevel
    void**		user;	// NULL if a free block
    struct memblock_s*	next;
    struct memblock_s*	prev;
} memblock_t;


typedef struct
{
    // start / end cap for linked list
    memblock_t	blocklist;
    memblock_t*	rover;
} memzone_t;

memzone_t*	mainzone;

#if defined(NUMWORKS) && PLATFORM_DEVICE
// Fallback when allocator-backed heap doesn't give nearly enough memory
#ifdef USE_UNSTABLE_ZONE_HEAP_SIZE
static byte s_numworks_zone_fallback_heap[101 * 1024] __attribute__((aligned(8)));
#else
static byte s_numworks_zone_fallback_heap[72 * 1024] __attribute__((aligned(8)));
#endif
#endif
static unsigned int Z_ProbeLargestAlloc(unsigned int startSize, unsigned int stepSize, unsigned int floorSize)
{
    unsigned int size = startSize;

    while (size >= floorSize)
    {
        void* probe = malloc(size);

        if (probe != NULL)
        {
            free(probe);
            return size;
        }

        if (size <= stepSize)
            break;

        size -= stepSize;
    }

    return 0;
}

//
// Z_Init
//
void Z_Init (void)
{
    memblock_t*	block;

    unsigned int heapSize = maxHeapSize;
    mainzone = NULL;

    // Try progressively smaller heaps but stop at a safe floor.
    while (heapSize >= minHeapSize)
    {
        mainzone = malloc(heapSize);

        if (mainzone != NULL)
            break;

        heapSize -= heapStepSize;
    }

    if (mainzone == NULL)
    {
#if defined(NUMWORKS) && PLATFORM_DEVICE
        mainzone = (memzone_t*)s_numworks_zone_fallback_heap;
        heapSize = sizeof(s_numworks_zone_fallback_heap);
        lprintf(LO_WARN, "Z_Init: malloc heap unavailable, using static fallback heap (%u bytes)", heapSize);
#else
           const unsigned int probeStart = (minHeapSize > heapStepSize) ? (minHeapSize - heapStepSize) : minHeapSize;
           const unsigned int largestProbe = Z_ProbeLargestAlloc(probeStart, heapStepSize, 4 * 1024);

           I_Error(
              "Z_Init: failed to allocate zone heap (min %u bytes)\n"
              "Target max: %u bytes\n"
              "Largest contiguous probe: %u bytes",
              minHeapSize,
              maxHeapSize,
              largestProbe);
#endif
    }

    lprintf(LO_INFO,"Z_Init: Heapsize is %u bytes.", heapSize);
    s_zone_heap_size = heapSize;

    // set the entire zone to one free block
    mainzone->blocklist.next =
    mainzone->blocklist.prev =
    block = (memblock_t *)( (byte *)mainzone + sizeof(memzone_t) );

    mainzone->blocklist.user = (void *)mainzone;
    mainzone->blocklist.tag = PU_STATIC;
    mainzone->rover = block;

    block->prev = block->next = &mainzone->blocklist;

    // NULL indicates a free block.
    block->user = NULL;

    block->size = heapSize - sizeof(memzone_t);
}

unsigned int Z_GetHeapSize(void)
{
    return s_zone_heap_size;
}

unsigned int Z_GetFreeMemory(void)
{
    unsigned int total = 0;
    memblock_t* block;

    if (mainzone == NULL)
        return 0;

    block = mainzone->blocklist.next;

    while (block != &mainzone->blocklist)
    {
        if (block->user == NULL)
            total += block->size;

        block = block->next;
    }

    return total;
}

unsigned int Z_GetAllocatedMemory(void)
{
    unsigned int free_bytes = Z_GetFreeMemory();

    if (s_zone_heap_size <= free_bytes)
        return 0;

    return s_zone_heap_size - free_bytes;
}


//
// Z_Free
//
void Z_Free (void* ptr)
{
    memblock_t*		block;
    memblock_t*		other;

    if(ptr == NULL)
        return;

    block = (memblock_t *) ( (byte *)ptr - sizeof(memblock_t));

    if (block->user > (void **)0x100)
    {
        // smaller values are not pointers
        // Note: OS-dependend?

        // clear the user's mark
        *block->user = 0;
    }

    // mark as free
    block->user = NULL;
    block->tag = 0;


#ifndef GBA
    running_count -= block->size;
    printf("Free: %d\n", running_count);
#endif

    other = block->prev;

    if (!other->user)
    {
        // merge with previous free block
        other->size += block->size;
        other->next = block->next;
        other->next->prev = other;

        if (block == mainzone->rover)
            mainzone->rover = other;

        block = other;
    }

    other = block->next;
    if (!other->user)
    {
        // merge the next free block onto the end
        block->size += other->size;
        block->next = other->next;
        block->next->prev = block;

        if (other == mainzone->rover)
            mainzone->rover = block;
    }
}



//
// Z_Malloc
// You can pass a NULL user if the tag is < PU_PURGELEVEL.
//
#define MINFRAGMENT		64


void* Z_Malloc(int size, int tag, void **user)
{
    int		extra;
    memblock_t*	start;
    memblock_t* rover;
    memblock_t* newblock;
    memblock_t*	base;

    size = (size + 3) & ~3;

    // scan through the block list,
    // looking for the first free block
    // of sufficient size,
    // throwing out any purgable blocks along the way.

    // account for size of block header
    size += sizeof(memblock_t);

    // if there is a free block behind the rover,
    //  back up over them
    base = mainzone->rover;

    if (!base->prev->user)
    base = base->prev;

    rover = base;
    start = base->prev;

    do
    {
        if (rover == start)
        {
            // scanned all the way around the list
    #ifndef GBA
                I_Error ("Z_Malloc: failed allocation of %i bytes\nUsed: %d bytes", size, running_count);
    #else
            I_Error ("Z_Malloc: failed on allocation of %i bytes", size);
    #endif
        }

        if (rover->user)
        {
            if (rover->tag < PU_PURGELEVEL)
            {
                // hit a block that can't be purged,
                //  so move base past it
                base = rover = rover->next;
            }
            else
            {
                // free the rover block (adding the size to base)

                // the rover can be the base block
                base = base->prev;
                Z_Free ((byte *)rover+sizeof(memblock_t));
                base = base->next;
                rover = base->next;
            }
        }
        else
            rover = rover->next;

    } while (base->user || base->size < size);


    // found a block big enough
    extra = base->size - size;

    if (extra >  MINFRAGMENT)
    {
        // there will be a free fragment after the allocated block
        newblock = (memblock_t *) ((byte *)base + size );
        newblock->size = extra;

        // NULL indicates free block.
        newblock->user = NULL;
        newblock->tag = 0;
        newblock->prev = base;
        newblock->next = base->next;
        newblock->next->prev = newblock;

        base->next = newblock;
        base->size = size;
    }

    if (user)
    {
        // mark as an in use block
        base->user = user;
        *(void **)user = (void *) ((byte *)base + sizeof(memblock_t));
    }
    else
    {
        if (tag >= PU_PURGELEVEL)
            I_Error ("Z_Malloc: an owner is required for purgable blocks");

        // mark as in use, but unowned
        base->user = (void *)2;
    }

    base->tag = tag;

    // next allocation will start looking here
    mainzone->rover = base->next;

#ifndef GBA
    running_count += base->size;
    printf("Alloc: %d (%d)\n", base->size, running_count);
#endif

    return (void *) ((byte *)base + sizeof(memblock_t));
}

void* Z_Calloc(size_t count, size_t size, int tag, void **user)
{
    const size_t bytes = count * size;
    void* ptr = Z_Malloc(bytes, tag, user);

    if(ptr)
        memset(ptr, 0, bytes);

    return ptr;
}

char* Z_Strdup(const char* s)
{
    const unsigned int len = strlen(s);

    if(!len)
        return NULL;

    char* ptr = Z_Malloc(len+1, PU_STATIC, NULL);

    if(ptr)
        strcpy(ptr, s);

    return ptr;
}

void* Z_Realloc(void *ptr, size_t n, int tag, void **user)
{
    void *p = Z_Malloc(n, tag, user);

    if (ptr)
    {
        memblock_t *block = (memblock_t *)((char *) ptr - sizeof(memblock_t));

        memcpy(p, ptr, n <= block->size ? n : block->size);

        Z_Free(ptr);

        if (user) // in case Z_Free nullified same user
            *user = p;
    }
    return p;
}

//
// Z_FreeTags
//
void Z_FreeTags(int lowtag, int hightag)
{
    memblock_t*	block;
    memblock_t*	next;

    for (block = mainzone->blocklist.next ;
         block != &mainzone->blocklist ;
         block = next)
    {
        // get link before freeing
        next = block->next;

        // free block?
        if (!block->user)
            continue;

        if (block->tag >= lowtag && block->tag <= hightag)
            Z_Free ( (byte *)block+sizeof(memblock_t));
    }
}

//
// Z_CheckHeap
//
void Z_CheckHeap (void)
{
    memblock_t*	block;

    for (block = mainzone->blocklist.next ; ; block = block->next)
    {
        if (block->next == &mainzone->blocklist)
        {
            // all blocks have been hit
            break;
        }

        if ( (byte *)block + block->size != (byte *)block->next)
            I_Error ("Z_CheckHeap: block size does not touch the next block\n");

        if ( block->next->prev != block)
            I_Error ("Z_CheckHeap: next block doesn't have proper back link\n");

        if (!block->user && !block->next->user)
            I_Error ("Z_CheckHeap: two consecutive free blocks\n");
    }
}

#endif
