#include "generated/eternalsonata_init.h"

// sub_8226F158 clears the GPU interrupt callback via VdSetGraphicsInterruptCallback(0,0)
// then immediately reinitialises the ring buffer. On real Xbox 360 hardware the interrupt
// line is masked during this window; the SDK dispatches it synchronously, so without this
// hook the first ring buffer interrupt dispatches to guest address 0 and crashes.
void EternalsonataPreserveInterruptCallback() {}

// sub_82294600 loads a sub-object from this+4 and calls its virtual destructor/release
// (vtable slot 10, offset 40). That vtable entry is not registered; skip the call
// so the function can proceed to zero out this+4 and this+20 as normal.
void EternalsonataSkipSubObjectRelease() {}

// sub_82160DC8 walks an element tree, resolving each entry against the global
// resource manager (sub_82176CD8, guest singleton 0x824CF500). On a failed lookup
// the game sets r31 = 0 and then *unconditionally* dereferences [r31+4] at guest
// 0x82160E70 (mr r30,r3; lwz r11,4(r31); clrlwi. r11,r11,31; beq loc_82160EE0).
// On console the lookup never fails, so r31 is never null there; in this port a
// resource can be missing (same root cause as the blank-text issue), making r31
// null and faulting the load. When the resolved entry is unusable the code's own
// next step is to skip it (the r11&1 test falls through to loc_82160EE0), so treat
// a null entry as "skip" instead of crashing. Returning true jumps to loc_82160EE0.
bool EternalsonataGuardNullResourceEntry(PPCRegister& r31) { return r31.u32 == 0; }
