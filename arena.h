#pragma once
#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ALIGN_UP_POW2
#define ALIGN_UP_POW2(n, p) (((u64)(n) + ((u64)(p) - 1)) & (~((u64)(p) - 1)))
#endif
#ifndef ARENA_BASE_POS
#define ARENA_BASE_POS (sizeof(mem_arena))
#endif
#ifndef ARENA_ALIGN
#define ARENA_ALIGN (sizeof(void *))
#endif

typedef struct mem_arena mem_arena;
struct mem_arena {
  u64 capacity;
  u64 pos;
};


#define PUSH_STRUCT(arena, T) (T *)arena_push((arena), sizeof(T), false)
#define PUSH_STRUCT_ZERO(arena, T) (T *)arena_push((arena), sizeof(T), true)
#define PUSH_ARRAY(arena, T, n) (T *)arena_push((arena), sizeof(T) * n, false)
#define PUSH_ARRAY_ZERO(arena, T, n) \
  (T *)arena_push((arena), sizeof(T) * n, true)

#define ARENA_SAVE_POS(arena) ((arena)->pos)
#define ARENA_RESTORE(arena, pos) arena_pop_to((arena), (pos), false)

mem_arena *arena_create(u64 capacity);
void arena_destroy(mem_arena *arena);
void *arena_push(mem_arena *arena, u64 size, b32 zero);
void arena_pop(mem_arena *arena, u64 size, b32 zero);
void arena_pop_to(mem_arena *arena, u64 new_pos, b32 zero);
void arena_clear(mem_arena *arena, b32 zero);
