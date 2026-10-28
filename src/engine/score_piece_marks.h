// eternalsonata - Score piece marks, any number at once, as on the PS3.
//
// The 360 keeps one Mark, a piece number at byte_8243FCF4; the PS3 keeps a
// u32 with bit n - 1 per piece at party block +0x928 (sub_232698 writes it,
// sub_234A18 reads it). The full set lives here and in the save record, and
// the guest byte follows it as the lowest marked piece, so a save taken back
// to a retail 360 keeps one of them. Nothing in the game reads the Mark
// outside the Score Pieces menu.
#pragma once

#include <cstdint>

#include "generated/eternalsonata_init.h"
#include "save_record.h"

namespace eternalsonata {

// Bit n - 1 set when piece n is marked.
uint32_t ScorePieceMarks();
void SetScorePieceMarks(uint32_t marks);

void SaveScorePieceMarks(SaveRecord& record);
void LoadScorePieceMarks(const SaveRecord& record);
void ResetScorePieceMarks();

}  // namespace eternalsonata

namespace score_piece_marks {

// From the sub_821EF0E8 hook: `before` is its context on entry.
void ImageMade(PPCContext& before, std::uint8_t* base, std::uint32_t image_id,
               std::uint32_t handle);
// From the sub_82179F78 hook, before the call.
void MirrorSlide(PPCContext& ctx, std::uint8_t* base);

}  // namespace score_piece_marks
