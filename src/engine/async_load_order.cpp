// eternalsonata - Publish an async load before the reader can see it.
//
// sub_8210CC68 queues a file on the reader thread, and only then stores the
// request id, destination and size on the file. The decoder thread drops every
// chunk of a file whose request id is still negative, so a reader that wins the
// race reads the whole file into nothing and the load never completes. Android
// preempts the caller right after it wakes the reader often enough to hang the
// title screen's title.bmd reload.
//
// Same function, with the stores moved ahead of the enqueue.

#include "generated/eternalsonata_init.h"

#include <rex/hook.h>

namespace {

constexpr u32 kReader = 0x82466108u;  // dword_82466108, the reader thread's state

// The file struct sub_8210C9D8 opens.
constexpr u32 kCompressed = 16;  // byte
constexpr u32 kPackedSize = 20;
constexpr u32 kSize = 24;
constexpr u32 kDest = 28;
constexpr u32 kRequested = 32;
constexpr u32 kPending = 36;
constexpr u32 kRequest = 320;

}  // namespace

REX_HOOK_RAW(sub_8210CC68) {
  const u32 file = ctx.r3.u32;
  const u32 dest = ctx.r4.u32;
  const u32 size = ctx.r5.u32;
  u32 offset = ctx.r6.u32;
  const u32 priority = ctx.r7.u32;

  const u32 handle = REX_LOAD_U32(file);
  if (!handle || handle == 0xFFFFFFFFu || REX_LOAD_U32(file + kPending)) {
    ctx.r3.u64 = 0;
    return;
  }
  u32 length = size;
  if (REX_LOAD_U8(file + kCompressed)) {
    if (size != REX_LOAD_U32(file + kSize) || offset) {
      ctx.r3.u64 = 0;
      return;
    }
    length = REX_LOAD_U32(file + kPackedSize);
    offset = 0;
  }

  // Any non-negative id lets the decoder take the chunks; the real one follows.
  REX_STORE_U32(file + kRequest, 0);
  REX_STORE_U32(file + kDest, dest);
  REX_STORE_U32(file + kRequested, size);
  REX_STORE_U32(file + kPending, size);

  PPCContext call = ctx;
  call.r3.u64 = kReader;
  call.r4.u64 = file;
  call.r5.u64 = dest;
  call.r6.u64 = length;
  call.r7.u64 = offset;
  call.r8.u64 = priority;
  sub_8210D5D8(call, base);
  REX_STORE_U32(file + kRequest, call.r3.u32);

  ctx.r3.u64 = 1;
}
