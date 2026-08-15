// eternalsonata - Party state: reading, writing, and the mod-facing API.
//
// The public C ABI mods call is eternalsonata_party_api.h; this header is the
// small internal surface the rest of the exe needs. The reverse-engineering
// behind it (every guest address and routine involved) is written up in
// docs/party-system.md.
#pragma once

#include <cstdint>

namespace rex {
class Runtime;
}  // namespace rex

namespace eternalsonata {

// Binds the party system to the runtime. Call once the runtime is live
// (OnPostSetup). Until then every API entry point answers "unavailable".
void BindPartySystem(rex::Runtime* runtime);

// Character-name override hook. The game resolves every on-screen character
// name through the BTX text lookup sub_8223B780, so renaming a character is a
// matter of answering that lookup with a different string: given the address
// the stock lookup returned, this returns the guest address of a replacement
// string, or 0 to leave the result alone. Called from the sub_8223B780 hook in
// eternalsonata_options.cpp (there can only be one hook per guest function).
uint32_t PartyNameOverrideFor(uint32_t text_address);

}  // namespace eternalsonata
