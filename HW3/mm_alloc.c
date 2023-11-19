/*
 * mm_alloc.c
 */

#include "mm_alloc.h"

#include <stdlib.h>
#include <unistd.h>
#include <string.h>

s_block_ptr start_block = NULL;

void *mm_malloc(size_t size)
{
    /* invalid size */
    if (size == 0)
        return NULL;

    /* try finding a block with size greater or equal to "size" */
    s_block_ptr last_block = NULL;
    for (s_block_ptr current_block = start_block;
         current_block != NULL;
         current_block = current_block->next)
    {
        last_block = current_block;
        if (current_block->is_free && current_block->size >= size)
        {
            /* mark block as used, split and return it */
            current_block->is_free = 0;
            split_block(current_block, size);
            memset(current_block->ptr, 0, size);
            return current_block->ptr;
        }
    }

    /* if no block is found create a new block by moving break */
    s_block_ptr new_block = extend_heap(last_block, size);
    if (new_block == NULL)
        return NULL;

    /* mark block as used, set memory to zero and return it */
    new_block->is_free = 0;
    memset(new_block->ptr, 0, size);
    return new_block->ptr;
}

void *mm_realloc(void *ptr, size_t size)
{
    /* check args */
    if (ptr == NULL)
    {
        if (size == 0)
            return NULL;
        else
            return mm_malloc(size);
    }
    else if (size == 0)
    {
        mm_free(ptr);
        return NULL;
    }

    /* try to get the block corresponding to the address "ptr" */
    s_block_ptr block = get_block(ptr);
    if (block == NULL)
        return NULL;

    /* try to allocate memory with new size */
    void *new_ptr = mm_malloc(size);
    if (new_ptr == NULL)
        return NULL;

    /* copy old memory content to new */
    ssize_t min_size = block->size;
    if (size < min_size)
        min_size = size;
    memcpy(new_ptr, ptr, min_size);

    /* mm_free old memory */
    mm_free(ptr);

    return new_ptr;
}

void mm_free(void *ptr)
{
    /* invalid pointer */
    if (ptr == NULL)
        return;

    /* try to get the block corresponding to the address "ptr" */
    s_block_ptr block = get_block(ptr);
    if (block == NULL)
        return;

    /* mark block as unused and fuse it */
    block->is_free = 1;
    fusion(block);
}

void split_block(s_block_ptr b, size_t s)
{
    /* check if block has enough extra space */
    if (b->size < s + BLOCK_SIZE + MIN_MEM_SIZE)
        return;

    /* split block into two blocks */
    s_block_ptr new_block = (void *)b + BLOCK_SIZE + s;
    new_block->size = b->size - (s + BLOCK_SIZE);
    b->size = s;
    new_block->prev = b;
    new_block->next = b->next;
    if (b->next != NULL)
        b->next->prev = new_block;
    b->next = new_block;
    new_block->is_free = 1;
    new_block->ptr = (void *)new_block + BLOCK_SIZE;
}

s_block_ptr fusion(s_block_ptr b)
{
    /* try to fuse the block with next block */
    s_block_ptr next_block = b->next;
    if (next_block != NULL && next_block->is_free)
    {
        b->size += BLOCK_SIZE + next_block->size;
        b->next = next_block->next;
        if (next_block->next != NULL)
            next_block->next->prev = b;
    }

    /* try to fuse the block with prev block */
    s_block_ptr prev_block = b->prev;
    if (prev_block != NULL && prev_block->is_free)
    {
        prev_block->size += BLOCK_SIZE + b->size;
        prev_block->next = b->next;
        if (b->next != NULL)
            b->next->prev = prev_block;

        return prev_block;
    }

    return b;
}

s_block_ptr get_block(void *p)
{
    for (s_block_ptr current_block = start_block;
         current_block != NULL;
         current_block = current_block->next)
    {
        if (current_block->ptr <= p && p < current_block->ptr + current_block->size)
            return current_block;
    }

    return NULL;
}

s_block_ptr extend_heap(s_block_ptr last, size_t s)
{
    size_t extend_size = s + BLOCK_SIZE;
    void *break_ptr = sbrk(0);

    s_block_ptr new_block = break_ptr;
    if (last != NULL)
        new_block = (void *)last + BLOCK_SIZE + last->size;

    /* check if we need to move break pointer */
    size_t available_space = break_ptr - (void *)new_block;
    if (available_space < extend_size)
    {
        /* try to move break pointer */
        void *old_break_ptr = sbrk(extend_size - available_space);
        if (old_break_ptr == (void *)-1)
            return NULL;
    }

    /* initialize new_block */
    new_block->size = s;
    if (last != NULL)
    {
        last->next = new_block;
        new_block->prev = last;
        new_block->next = NULL;
    }
    else
    {
        start_block = new_block;
        new_block->prev = NULL;
        new_block->next = NULL;
    }
    new_block->is_free = 1;
    new_block->ptr = (void *)new_block + BLOCK_SIZE;

    return new_block;
}
