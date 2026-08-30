// ue_wrap/core/bool_mask.h -- byte-preserving UE bool-property writes (pure; no engine).
//
// WHY: UE packs several `uint8 flag : 1` bitfields into shared bytes. A bool property's
// reflection descriptor is {ArrayIndex, ByteOffset, ByteMask, FieldMask} -- the property owns
// ONLY its mask bits inside the byte, so a whole-byte overwrite clobbers its packed
// neighbors (the 2026-07-03 lifeLight XOR-heuristic failure is the cautionary record).
// reflection::FindBoolProperty resolves (offset, mask) precisely so callers can write
// mask-preservingly; this header holds the pure byte math so it is testable headless
// (HARDENING Q, v3 corrective pass).
//
//   byte = (byte & ~mask) | mask      -- set the property's bits, leave every other bit alone
//   (algebraically identical to `byte | mask` for a set; kept explicit in tests, not here --
//   SetBoolMaskBits IS the production primitive).

#pragma once

#include <cstdint>

namespace ue_wrap::bool_mask {

// Set every bit of `mask` inside `byte`, preserving all unrelated bits.
inline uint8_t SetBoolMaskBits(uint8_t byte, uint8_t mask) {
    return static_cast<uint8_t>(static_cast<uint8_t>(byte & static_cast<uint8_t>(~mask)) | mask);
}

// Clear every bit of `mask` inside `byte`, preserving all unrelated bits. (Provided for the
// rare "write false" case; the takenByDrone retire path only ever sets true.)
inline uint8_t ClearBoolMaskBits(uint8_t byte, uint8_t mask) {
    return static_cast<uint8_t>(byte & static_cast<uint8_t>(~mask));
}

}  // namespace ue_wrap::bool_mask
