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
