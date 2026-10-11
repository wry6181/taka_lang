#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef int64_t i64;
typedef int32_t i32;
typedef int16_t i16;
typedef int8_t i8;
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef float f32;
typedef double f64;
typedef i32 b32;
typedef i8 b8;
typedef atomic_uint au32;

typedef struct {
  u8 *data;
  u64 size;
} String8;

#define Byte(n) (u64)(n)
#define KByte(n) ((u64)(n) << 10)
#define MByte(n) ((u64)(n) << 20)
#define GByte(n) ((u64)(n) << 30)

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

#define UNUSED(x) ((void)(x))
// #define UNREACHABLE() ASSERT(!"Unreachable code!");
// #define NOTINPLAMENTED() ASSERT(!"Not inplamented yet!")

#define NUM_ARGS(...) (sizeof((int[]){__VA_ARGS__}) / sizeof(int))

#define BIT_FLAG_ENABLE(flags_addr, ...)            \
  do {                                              \
    u32 _bits[] = {__VA_ARGS__};                    \
    u32 _mask = 0;                                  \
    for (int i = 0; i < NUM_ARGS(__VA_ARGS__); ++i) \
      _mask |= _bits[i];                            \
    *(flags_addr) |= _mask;                         \
  } while (0)

#define BIT_FLAG_DISABLE(flags_addr, ...)           \
  do {                                              \
    u32 _bits[] = {__VA_ARGS__};                    \
    u32 _mask = 0;                                  \
    for (int i = 0; i < NUM_ARGS(__VA_ARGS__); ++i) \
      _mask |= _bits[i];                            \
    *(flags_addr) &= ~_mask;                        \
  } while (0)

#define BIT_FLAG_TOGGLE(flags_addr, ...)            \
  do {                                              \
    u32 _bits[] = {__VA_ARGS__};                    \
    u32 _mask = 0;                                  \
    for (int i = 0; i < NUM_ARGS(__VA_ARGS__); ++i) \
      _mask |= _bits[i];                            \
    *(flags_addr) ^= _mask;                         \
  } while (0)

#define BIT_FLAG_ANY(flags, ...) ({               \
  u32 _bits[] = {__VA_ARGS__};                    \
  u32 _mask = 0;                                  \
  for (int i = 0; i < NUM_ARGS(__VA_ARGS__); ++i) \
    _mask |= _bits[i];                            \
  (flags) & _mask;                                \
})

#define BIT_FLAG_ALL(flags, ...) ({               \
  u32 _bits[] = {__VA_ARGS__};                    \
  u32 _mask = 0;                                  \
  for (int i = 0; i < NUM_ARGS(__VA_ARGS__); ++i) \
    _mask |= _bits[i];                            \
  ((flags) & _mask) == _mask;                     \
})

#define BIT_FLAG_EXACT(flags, mask) (((flags) & (mask)) == (mask))

#define BIT_FLAG_SET_WITH_COND(flags, bit, cond) \
  ((flags) = ((flags) & ~(bit)) | (-!!(cond) & (bit)))

// Properly define static assertions.
#if defined(__clang__) || defined(__gcc__)
#  define STATIC_ASSERT _Static_assert
#else
#  define STATIC_ASSERT static_assert
#endif

// Ensure all types are of the correct size.
STATIC_ASSERT(sizeof(u8) == 1, "Expected u8 to be 1 byte.");
STATIC_ASSERT(sizeof(u16) == 2, "Expected u16 to be 2 bytes.");
STATIC_ASSERT(sizeof(u32) == 4, "Expected u32 to be 4 bytes.");
STATIC_ASSERT(sizeof(u64) == 8, "Expected u64 to be 8 bytes.");

STATIC_ASSERT(sizeof(i8) == 1, "Expected i8 to be 1 byte.");
STATIC_ASSERT(sizeof(i16) == 2, "Expected i16 to be 2 bytes.");
STATIC_ASSERT(sizeof(i32) == 4, "Expected i32 to be 4 bytes.");
STATIC_ASSERT(sizeof(i64) == 8, "Expected i64 to be 8 bytes.");

STATIC_ASSERT(sizeof(f32) == 4, "Expected f32 to be 4 bytes.");
STATIC_ASSERT(sizeof(f64) == 8, "Expected f64 to be 8 bytes.");

#define TRUE 1
#define FALSE 0

// Platform detection
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__)
#  define T_PLATFORM_WINDOWS 1
#  ifndef _WIN64
#    error "64-bit is required on Windows!"
#  endif
#elif defined(__linux__) || defined(__gnu_linux__)
// Linux OS
#  define T_PLATFORM_LINUX 1
#  if defined(__ANDROID__)
#    define T_PLATFORM_ANDROID 1
#  endif
#elif defined(__unix__)
// Catch anything not caught by the above.
#  define T_PLATFORM_UNIX 1
#elif defined(_POSIX_VERSION)
// Posix
#  define T_PLATFORM_POSIX 1
#elif __APPLE__
#  define T_PLATFORM_APPLE 1
#  define T_PLATFORM_MAC 1
#  include <TargetConditionals.h>
#  if TARGET_IPHONE_SIMULATOR
// iOS Simulator
#    define T_PLATFORM_IOS 1
#    define T_PLATFORM_IOS_SIMULATOR 1
#  elif TARGET_OS_IPHONE
#    define T_PLATFORM_IOS 1
// iOS device
#  elif TARGET_OS_MAC
// Other kinds of Mac OS
#  else
#    error "Unknown Apple platform"
#  endif
#else
#  error "Unknown platform!"
#endif

#ifdef T_EXPORT
// Exports
#  ifdef _MSC_VER
#    define TAKI __declspec(dllexport)
#  else
#    define TAKI __attribute__((visibility("default")))
#  endif
#else
// Imports
#  ifdef _MSC_VER
#    define TAKI __declspec(dllimport)
#  else
#    define TAKI
#  endif
#endif
