#include "arena.h"
#include <string.h>

mem_arena *arena_create(u64 capacity){
    u64 size =  ALIGN_UP_POW2(sizeof(mem_arena) + capacity, ARENA_ALIGN);
    mem_arena* arena = (mem_arena*)malloc(size);
    if(!arena) {
        return NULL;
    }
    arena->pos = 0;
    arena->capacity = size - sizeof(mem_arena);
    return arena;
}
void arena_destroy(mem_arena *arena){
    if(!arena) {
        return;
    }
    free(arena);
}
void *arena_push(mem_arena *arena, u64 size, b32 zero){
    if(!arena) {
        return NULL;
    }
    u64 new_size = ALIGN_UP_POW2(size, ARENA_ALIGN);
    u64 old_pos = arena->pos;
    u64 new_pos = old_pos + new_size;
    if(new_pos > arena->capacity) {
        return NULL;
    }
    if(zero) {
        memset((u8*)arena + old_pos + ARENA_BASE_POS, 0, new_size);
    }
    arena->pos = new_pos;
    return (u8*)arena + old_pos + ARENA_BASE_POS;

}
void arena_pop(mem_arena *arena, u64 size, b32 zero){
    if(!arena) {
        return;
    }
    size = ALIGN_UP_POW2(size, ARENA_ALIGN);
    u64 old_pos = arena->pos;
    u64 new_pos = 0;
    if(old_pos > size) {
        new_pos = old_pos - size;
    }
    if(zero) {
        memset((u8*)arena + new_pos + ARENA_BASE_POS, 0, old_pos - new_pos);
    }
    arena->pos = new_pos;
}
void arena_pop_to(mem_arena *arena, u64 new_pos, b32 zero){
    if(!arena) {
        return;
    }
    u64 old_pos = arena->pos;
    if(old_pos < new_pos) {
        return;
    }
    u64 size = old_pos - new_pos;
    if(zero) {
        memset((u8*)arena + new_pos + ARENA_BASE_POS, 0, size);
    }
    arena->pos = new_pos;
}
void arena_clear(mem_arena *arena, b32 zero){
    if(!arena) {
        return;
    }
    arena_pop_to(arena, 0, zero);
}
