/*
 * Id:
 * Portable Audio I/O Library allocation context implementation
 * memory allocation context for tracking allocation groups
 *
 * Based on the Open Source API proposed by Ross Bencina
 * Copyright (c) 1999-2002 Ross Bencina, Phil Burk
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * Any person wishing to distribute modifications to the Software is
 * requested to send the modifications to the original developer so that
 * they can be incorporated into the canonical version.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
 
#include "pa_allocation.h"
#include "pa_util.h"

/*
    Maintain 3 singly linked lists...
    linkBlocks: the buffers used to allocate the links
    spareLinks: links available for use in the allocations list
    allocations: the buffers currently allocated using PaUtil_ContextAllocateMemory()

    Link block size is doubled every time new links are allocated.
*/


#define PA_INITIAL_LINK_COUNT_    16

struct PaUtilAllocationContextLink
{
    void *buffer;
    struct PaUtilAllocationContextLink *next;
};

/*
    Allocate a block of links. The first link will have it's buffer member
    pointing to the block, and it's next member set to <nextBlock>. The remaining
    links will have NULL buffer members, and each link will point to
    the next link except the last, which will point to <nextSpare>
*/
static struct PaUtilAllocationContextLink *AllocateLinks( long count,
        struct PaUtilAllocationContextLink *nextBlock,
        struct PaUtilAllocationContextLink *nextSpare )
{
    struct PaUtilAllocationContextLink *result;
    int i;
    
    result = PaUtil_AllocateMemory( sizeof(struct PaUtilAllocationContextLink) * count );
    if( result )
    {
        /* the block link */
        result[0].buffer = result;
        result[0].next = nextBlock;

        /* the spare links */
        for( i=1; i<count; ++i )
        {
            result[i].buffer = 0;
            result[i].next = &result[i+1];
        }
        result[count-1].next = nextSpare;
    }
    
    return result;
}


PaUtilAllocationContext* PaUtil_CreateAllocationContext( void )
{
    PaUtilAllocationContext* result = 0;
    struct PaUtilAllocationContextLink *links;


    links = AllocateLinks( PA_INITIAL_LINK_COUNT_, 0, 0 );
    if( links != 0 )
    {
        result = (PaUtilAllocationContext*)PaUtil_AllocateMemory( sizeof(PaUtilAllocationContext) );
        if( result )
        {
            result->linkCount = PA_INITIAL_LINK_COUNT_;
            result->linkBlocks = &links[0];
            result->spareLinks = &links[1];
            result->allocations = 0;
        }
        else
        {
            PaUtil_FreeMemory( links );
        }
    }

    return result;
}


void PaUtil_DestroyAllocationContext( PaUtilAllocationContext* context )
{
    struct PaUtilAllocationContextLink *current = context->linkBlocks;
    struct PaUtilAllocationContextLink *next;

    while( current )
    {
        next = current->next;
        PaUtil_FreeMemory( current->buffer );
        current = next;
    }

    PaUtil_FreeMemory( context );
}


void* PaUtil_ContextAllocateMemory( PaUtilAllocationContext* context, long size )
{
    struct PaUtilAllocationContextLink *links, *link;
    void *result = 0;
    
    /* allocate more links if necessary */
    if( !context->spareLinks )
    {
        /* double the link count on each block allocation */
        links = AllocateLinks( context->linkCount, context->linkBlocks, context->spareLinks );
        if( links )
        {
            context->linkCount += context->linkCount;
            context->linkBlocks = &links[0];
            context->spareLinks = &links[1];
        }
    }

    if( context->spareLinks )
    {
        result = PaUtil_AllocateMemory( size );
        if( result )
        {
            link = context->spareLinks;
            context->spareLinks = link->next;

            link->buffer = result;
            link->next = context->allocations;

            context->allocations = link;
        }
    }

    return result;    
}


void PaUtil_ContextFreeMemory( PaUtilAllocationContext* context, void *buffer )
{
    struct PaUtilAllocationContextLink *current = context->allocations;
    struct PaUtilAllocationContextLink *previous = 0;

    if( buffer == 0 )
        return;

    /* find the right link and remove it */
    while( current )
    {
        if( current->buffer == buffer )
        {
            previous->next = current->next;

            current->buffer = 0;
            current->next = context->spareLinks;
            context->spareLinks = current;
        }
        previous = current;
        current = current->next;
    }

    PaUtil_FreeMemory( buffer ); /* free the memory whether we found it in the list or not */
}


void PaUtil_FreeAllAllocations( PaUtilAllocationContext* context )
{
    struct PaUtilAllocationContextLink *current = context->allocations;
    struct PaUtilAllocationContextLink *previous = 0;

    /* free all buffers in the allocations list */
    while( current )
    {
        PaUtil_FreeMemory( current->buffer );
        current->buffer = 0;

        previous = current;
        current = current->next;
    }

    /* link the former allocations list onto the front of the spareLinks list */
    if( previous )
    {
        previous->next = context->spareLinks;
        context->spareLinks = context->allocations;
        context->allocations = 0;
    }
}

