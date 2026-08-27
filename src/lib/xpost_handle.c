/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_handle.c
 * @brief Handles: naming a block that is not in virtual memory.
 *
 * A device's instance state and a font's face live outside the arena, and an
 * entity that names one carries a handle this process issued. That is why an
 * image of virtual memory is refused where any entity carries one: the
 * number would mean nothing in the process that read it back.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <string.h>

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_context.h"
#include "xpost_error.h"
#include "xpost_dict.h"
#include "xpost_string.h"
#include "xpost_name.h"
#include "xpost_handle.h"

/* One recorded block: the entity carrying its handle, the dictionary it
   was issued to where there is one, and what it holds. */
typedef struct
{
    Xpost_Memory_File *mem;      /* memory file of the handle entity */
    unsigned int ent;            /* handle entity; zero marks a free slot */
    Xpost_Memory_File *ownermem; /* memory file of the dictionary */
    unsigned int owner;          /* entity of the dictionary */
    Xpost_Handle_Kind kind;      /* what the block holds */
    unsigned int size;           /* bytes the block holds */
    unsigned int release;        /* opcode of a device block's release, or zero */
    /* what to give up out of the block before the block itself, where
       the block names memory of its own; run when the entity carrying
       the handle is reclaimed, which is the one moment a holder that
       was never told to give up gets to */
    void (*reclaim)(void *block);
    int held;                    /* the block is the holder's to free */
    void *block;
} Xpost_Handle_Slot;

/* The record of what has been issued. Slot zero is never issued, so the
   handle a string carries before anything is written into it -- and the
   handle in a string of the program's own making -- names nothing. */
static Xpost_Handle_Slot *_slots;
static unsigned int _nslots;

/* Take a free slot, growing the record when none is left. Returns zero
   when there is no memory for one. */
static unsigned int _slot_alloc(void)
{
    unsigned int i;
    unsigned int max;
    void *tmp;

    for (i = 1; i < _nslots; i++)
        if (_slots[i].ent == 0)
            return i;

    max = _nslots ? _nslots * 2 : 8;
    tmp = realloc(_slots, max * sizeof(*_slots));
    if (!tmp)
        return 0;
    _slots = (Xpost_Handle_Slot *)tmp;
    memset(&_slots[_nslots], 0, (max - _nslots) * sizeof(*_slots));
    i = _nslots ? _nslots : 1;
    _nslots = max;
    return i;
}

/* The slot an entity's handle names, or NULL. The handle is read from
   the entity rather than from the object naming it, so a substring and
   the string it came from answer the same, and it is checked back
   against the entity it was read from: a handle carrying the number of
   a slot issued elsewhere names that slot's entity, not this one. */
static Xpost_Handle_Slot *_slot_of(Xpost_Memory_File *mem,
                                        unsigned int ent)
{
    unsigned int index;

    if (!xpost_memory_get(mem, ent, 0, sizeof(index), &index))
        return NULL;
    if ((index == 0) || (index >= _nslots))
        return NULL;
    if ((_slots[index].mem != mem) || (_slots[index].ent != ent))
        return NULL;
    return &_slots[index];
}

/* Take a slot for a block, store its number in the entity that is to
   carry the handle, and fill the slot in. Returns the slot, or NULL
   where there is no slot to take or the entity will not hold the
   number -- in which case nothing has been recorded and the entity has
   not been written. */
static Xpost_Handle_Slot *_slot_record(Xpost_Memory_File *mem,
                                       unsigned int ent,
                                       Xpost_Handle_Kind kind,
                                       size_t size,
                                       int held,
                                       void *block)
{
    unsigned int index;

    index = _slot_alloc();
    if (index == 0)
    {
        XPOST_LOG_ERR("cannot record a block of state");
        return NULL;
    }
    if (!xpost_memory_put(mem, ent, 0, sizeof index, &index))
    {
        XPOST_LOG_ERR("cannot store a handle");
        return NULL;
    }
    _slots[index].mem = mem;
    _slots[index].ent = ent;
    _slots[index].ownermem = NULL;
    _slots[index].owner = 0;
    _slots[index].kind = kind;
    _slots[index].size = (unsigned int)size;
    _slots[index].release = 0;
    _slots[index].reclaim = NULL;
    _slots[index].held = held;
    _slots[index].block = block;
    return &_slots[index];
}

int xpost_handle_hold(Xpost_Memory_File *mem,
                      unsigned int ent,
                      Xpost_Handle_Kind kind,
                      size_t size,
                      void *block)
{
    return _slot_record(mem, ent, kind, size, 1, block) != NULL;
}

void *xpost_handle_block_at(Xpost_Memory_File *mem,
                            unsigned int ent,
                            Xpost_Handle_Kind kind,
                            size_t size)
{
    Xpost_Handle_Slot *slot = _slot_of(mem, ent);

    if (!slot || (slot->kind != kind) || (slot->size != size))
        return NULL;
    return slot->block;
}

int xpost_handle_drop(Xpost_Memory_File *mem, unsigned int ent)
{
    Xpost_Handle_Slot *slot = _slot_of(mem, ent);
    unsigned int none = 0;

    if (slot)
        memset(slot, 0, sizeof(*slot));
    return xpost_memory_put(mem, ent, 0, sizeof none, &none);
}

int xpost_handle_cons(Xpost_Context *ctx,
                      Xpost_Object dic,
                      Xpost_Object key,
                      Xpost_Object *anchor,
                      Xpost_Handle_Kind kind,
                      size_t size,
                      void (*reclaim)(void *block))
{
    Xpost_Memory_File *mem;
    Xpost_Handle_Slot *slot;
    Xpost_Object o;
    unsigned int ent;
    unsigned int tag;
    int owner;
    void *block;

    owner = xpost_object_get_ent(dic);
    if (owner < 0)
        return unregistered;

    block = calloc(1, size);
    if (!block)
    {
        XPOST_LOG_ERR("cannot allocate a block of state");
        return VMerror;
    }

    /* the handle is read-only to the program: what it names is checked
       either way, and a handle that cannot be overwritten in place is
       one fewer thing for the check to answer */
    o = xpost_object_cvlit(xpost_string_cons(ctx, XPOST_HANDLE_ENTITY_SIZE,
                                             NULL));
    if (xpost_object_get_type(o) != stringtype)
    {
        free(block);
        XPOST_LOG_ERR("cannot allocate a handle");
        return VMerror;
    }
    mem = xpost_context_select_memory(ctx, o);
    ent = (unsigned int)xpost_object_get_ent(o);
    slot = _slot_record(mem, ent, kind, size, 0, block);
    if (!slot ||
        !xpost_memory_table_get_tag(mem, ent, &tag) ||
        !xpost_memory_table_set_tag(mem, ent,
                                    tag | XPOST_MEMORY_TABLE_TAG_HANDLE))
    {
        if (slot)
            memset(slot, 0, sizeof(*slot));
        free(block);
        XPOST_LOG_ERR("cannot store a handle");
        return VMerror;
    }

    slot->reclaim = reclaim;
    slot->ownermem = xpost_context_select_memory(ctx, dic);
    slot->owner = (unsigned int)owner;

    /* A device's instance state is released by an operator its class
       installs, and a released one runs where no error is caught, so the
       operator run is not the program's to name. It is taken here, from
       the dictionary the device's own Create has just filled and the
       program has not yet reached, and held against a later reading of
       the same slot: what the program writes under /Destroy afterwards is
       not what the state was issued to be released by. */
    if (kind == XPOST_HANDLE_DEVICE)
    {
        Xpost_Object destroy = xpost_dict_get(ctx, dic,
                                              xpost_name_cons(ctx, "Destroy"));

        if (xpost_object_get_type(destroy) == operatortype)
            slot->release = destroy.mark_.padw;
    }

    o = xpost_object_set_access(ctx, o, XPOST_OBJECT_TAG_ACCESS_READ_ONLY);
    *anchor = o;
    return xpost_dict_put(ctx, dic, key, o);
}

/* The slot a handle names, of the kind and size asked for. */
static Xpost_Handle_Slot *_slot_named(Xpost_Context *ctx,
                                      Xpost_Object anchor,
                                      Xpost_Handle_Kind kind,
                                      size_t size)
{
    Xpost_Handle_Slot *slot;
    int ent;

    if (xpost_object_get_type(anchor) != stringtype)
        return NULL;
    ent = xpost_object_get_ent(anchor);
    if (ent < 0)
        return NULL;
    slot = _slot_of(xpost_context_select_memory(ctx, anchor),
                    (unsigned int)ent);
    if (!slot || (slot->kind != kind) || (slot->size != size))
        return NULL;
    return slot;
}

XPOST_NOINLINE
void *xpost_handle_block(Xpost_Context *ctx,
                         Xpost_Object anchor,
                         Xpost_Handle_Kind kind,
                         size_t size)
{
    Xpost_Handle_Slot *slot = _slot_named(ctx, anchor, kind, size);

    return slot ? slot->block : NULL;
}

XPOST_NOINLINE
void *xpost_handle_block_of(Xpost_Context *ctx,
                            Xpost_Object anchor,
                            Xpost_Object dic,
                            Xpost_Handle_Kind kind,
                            size_t size)
{
    Xpost_Handle_Slot *slot = _slot_named(ctx, anchor, kind, size);
    int ent;

    if (!slot)
        return NULL;
    ent = xpost_object_get_ent(dic);
    if ((ent < 0) ||
        (slot->ownermem != xpost_context_select_memory(ctx, dic)) ||
        (slot->owner != (unsigned int)ent))
        return NULL;
    return slot->block;
}

XPOST_NOINLINE
unsigned int xpost_handle_device_release(Xpost_Context *ctx,
                                         Xpost_Object dic)
{
    Xpost_Memory_File *mem;
    unsigned int i;
    int ent;

    ent = xpost_object_get_ent(dic);
    if (ent < 0)
        return 0;
    mem = xpost_context_select_memory(ctx, dic);
    for (i = 1; i < _nslots; i++)
        if ((_slots[i].ent != 0) &&
            (_slots[i].kind == XPOST_HANDLE_DEVICE) &&
            (_slots[i].ownermem == mem) &&
            (_slots[i].owner == (unsigned int)ent))
            return _slots[i].release;
    return 0;
}

void xpost_handle_release_entity(Xpost_Memory_File *mem,
                                      unsigned int ent)
{
    Xpost_Handle_Slot *slot = _slot_of(mem, ent);

    if (!slot)
        return;
    /* What the block names goes with the block. A device's state is
       given up by its Destroy, which the run makes; a device the run
       never made it to -- one a restore took back, or one nothing named
       by the time a collection came round -- is never told, and what it
       held would stay held for the life of the process. This is that
       device's last word. */
    if (slot->reclaim && slot->block)
        slot->reclaim(slot->block);
    /* a block the holder keeps goes when the holder says so: the file
       layer frees a stream's struct once nothing reads through it, which
       can be after the entity naming it has gone */
    if (!slot->held)
        free(slot->block);
    memset(slot, 0, sizeof(*slot));
}

/* The blocks the arena no longer names. Read the number out of each
   recorded entity and give the slot up where the entity does not carry
   it any more -- the entity was dropped, or the bytes that held the
   number were written over by whatever the revert put back there. The
   read is bounded here rather than by the memory layer's own check,
   which reports a rangecheck for an entity outside the table; an entity
   the revert un-counted is the ordinary case here and not a fault to
   report. */
void xpost_handle_release_orphans(Xpost_Memory_File *mem)
{
    unsigned int i;

    for (i = 1; i < _nslots; i++)
    {
        unsigned int index;
        unsigned int ent = _slots[i].ent;

        if ((ent == 0) || (_slots[i].mem != mem))
            continue;
        if (xpost_ent_valid(mem, ent) &&
            (mem->table.tab[ent].sz >= sizeof(index)))
        {
            memcpy(&index, (unsigned char *)xpost_ent_ptr(mem, ent),
                   sizeof(index));
            if (index == i)
                continue;
        }
        /* what the block names goes with the block, as it does at a
           reclamation: the device whose Destroy the job never reached
           gets its last word here */
        if (_slots[i].reclaim && _slots[i].block)
            _slots[i].reclaim(_slots[i].block);
        if (!_slots[i].held)
            free(_slots[i].block);
        memset(&_slots[i], 0, sizeof(_slots[i]));
    }
}

void xpost_handle_release_memory_file(Xpost_Memory_File *mem)
{
    unsigned int i;

    for (i = 1; i < _nslots; i++)
        if ((_slots[i].ent != 0) && (_slots[i].mem == mem))
        {
            /* what the block names goes with the block here as it does
               at a reclamation: a run that ends without retiring
               everything it made is the ordinary way a run ends */
            if (_slots[i].reclaim && _slots[i].block)
                _slots[i].reclaim(_slots[i].block);
            if (!_slots[i].held)
                free(_slots[i].block);
            memset(&_slots[i], 0, sizeof(_slots[i]));
        }
}
