#pragma once

// Protected PS3 save files, as a console writes them: each file listed in the
// save's PARAM.PFD is encrypted with a key that entry holds, sealed with the
// game's secure file id. RPCS3 keeps saves plain; scripts/convert_save.py
// has the same steps.

#include <cstdint>
#include <string>
#include <vector>

namespace eternalsonata {

// Decrypts `data`, the file `name` of the save whose PARAM.PFD is `pfd`, in
// place. False when the PFD is malformed or does not list `name`.
bool DecryptPs3ProtectedFile(const std::vector<uint8_t>& pfd, const std::string& name,
                             std::vector<uint8_t>& data);

}  // namespace eternalsonata
