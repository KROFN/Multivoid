// coop/props/container_extract_wire.h -- KROFNE FORK (2133): the container-contents BLOB
// grammar, carved out of container_contents_sync.cpp in the v3 corrective pass (blocker K).
//
// WHY A HEADER: the pack/parse pair is PURE byte work (no engine access), and blocker K is a
// PARSER bug -- "the corrective selftest must run the real algorithm, not grep for it". A pure
// header lets tests/batch1_corrective_selftest.cpp compile the EXACT production packer and the
// EXACT production parser headless (g++, no UE) and drive the full round-trip matrix against
// them. The game TU (container_contents_sync.cpp) consumes the same functions, so the tested
// code IS the shipped code.
//
// GRAMMAR (byte-identical to the v2 fork layout -- the content hash depends on it):
//   [u8 op]                         0 = stock b133 contents, 1 = fork extraction pack
//   [u32 eid]        little-endian
//   [u64 baseHash]   little-endian
//   [u16 nRecords]   little-endian
//   [nRecords x record]             the coop/items/save_record_wire per-record grammar
//   op=1 tail only:
//   [u8 tokenCount]                 1..kMaxTokensPerWrite
//   [tokenCount x u64] little-endian, each != 0
//
// BLOCKER K (the reason this exists): v2 parsed the op=1 tail by READING BACKWARDS from the end
// of the blob and treating the LAST BYTE as the token count. The last byte of the blob is the
// most-significant byte of the LAST TOKEN -- for normal session tokens (1, 2, 3, ... stored
// little-endian) that byte is 0, so v2 read count=0 and rejected EVERY well-formed op=1 blob as
// malformed. A deterministic, always-on parser bug. The v3 parser is STRUCTURAL and FORWARD:
// header -> exactly nRecords -> tail at the CURRENT offset -> require exact final offset. The
// full blob is validated (or rejected wholesale) BEFORE any caller-side side effect (the host
// CAS, pairing state, applies) can run.

#pragma once

#include "coop/items/save_record_wire.h"   // the record codec + Feasible (pure; no engine)

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coop::props::container_extract_wire {

// Blob ops. kOpContents = 0 is the b133 grammar; kOpContentsExtract = 1 is the FORK grammar
// (the same layout plus the extraction-token tail).
inline constexpr uint8_t kOpContents        = 0;
inline constexpr uint8_t kOpContentsExtract = 1;
// A take burst larger than this spills its tokens to the NEXT write (client-side queue cap).
inline constexpr size_t  kMaxTokensPerWrite = 4;

// ---- little-endian primitives (the blob grammar's own; the record codec has its own) ----------
// Byte-identical to the file-local helpers v2 used (standard LE shift math).

inline void AppU16LE(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

inline void AppU64LE(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}

inline bool RdU16LE(const std::vector<uint8_t>& b, size_t& o, uint16_t& v) {
    if (o + 2 > b.size()) return false;
    v = static_cast<uint16_t>(b[o] | (b[o + 1] << 8));
    o += 2;
    return true;
}

inline bool RdU64LE(const std::vector<uint8_t>& b, size_t& o, uint64_t& v) {
    if (o + 8 > b.size()) return false;
    v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(b[o + i]) << (i * 8);
    o += 8;
    return true;
}

// ---- pack ------------------------------------------------------------------------------------

// The op=0 pack (the b133 grammar). NOTE: this exact byte layout is ALSO the CONTENT HASH
// input -- ContentHash(eid, recs) = Fnv64(PackContents(eid, 0, recs)). Changing the layout
// shifts every published hash; do not touch it without a protocol decision.
inline std::vector<uint8_t> PackContents(uint32_t eid, uint64_t baseHash,
                                         const std::vector<ue_wrap::save_record::SaveRecord>& recs) {
    namespace W = coop::save_record_wire;
    std::vector<uint8_t> b;
    b.push_back(kOpContents);
    W::AppU32(b, eid);
    AppU64LE(b, baseHash);
    AppU16LE(b, static_cast<uint16_t>(recs.size()));
    for (const auto& r : recs) W::SerSave(b, r);
    return b;
}

// The op=1 pack -- identical records layout plus the extraction-token tail (the fork's
// contents-write <-> birth-intent coupling).
inline std::vector<uint8_t> PackContentsExtract(
        uint32_t eid, uint64_t baseHash,
        const std::vector<ue_wrap::save_record::SaveRecord>& recs,
        const std::vector<uint64_t>& tokens) {
    namespace W = coop::save_record_wire;
    std::vector<uint8_t> b;
    b.push_back(kOpContentsExtract);
    W::AppU32(b, eid);
    AppU64LE(b, baseHash);
    AppU16LE(b, static_cast<uint16_t>(recs.size()));
    for (const auto& r : recs) W::SerSave(b, r);
    b.push_back(static_cast<uint8_t>(tokens.size()));
    for (uint64_t t : tokens) AppU64LE(b, t);
    return b;
}

// ---- parse (BLOCKER K fix: structural, forward, exact) ----------------------------------------

struct ParsedBlob {
    uint32_t eid = 0;
    uint64_t baseHash = 0;
    std::vector<ue_wrap::save_record::SaveRecord> recs;
    bool                   hasTail = false;   // op == kOpContentsExtract
    std::vector<uint64_t>  tokens;            // 1..kMaxTokensPerWrite entries when hasTail
};

// Parse the WHOLE blob structurally and exactly:
//   header -> exactly nRecords records -> (op=1) the tail at the CURRENT offset
//   -> require the final offset to equal blob.size().
// ANY deviation (overrun, count out of range, zero token, MISSING bytes, EXTRA trailing
// bytes) fails the parse and the blob is rejected WHOLESALE. No back-reading, no ambiguity:
// the token count sits at a defined offset (right after the record stream), so it is read
// FORWARD like every other field.
//
// `maxRecords` is the caller's container cap (kMaxRecordsPerContainer in the game TU) -- a
// declared record count above it is rejected here, before any allocation.
inline bool ParseContentsBlob(const std::vector<uint8_t>& blob, size_t maxRecords, ParsedBlob& out) {
    namespace W = coop::save_record_wire;
    out = ParsedBlob{};
    size_t o = 0;
    uint8_t op = 0;
    if (!W::RdU8(blob, o, op)) return false;
    if (op != kOpContents && op != kOpContentsExtract) return false;   // unknown op
    if (!W::RdU32(blob, o, out.eid)) return false;
    if (!RdU64LE(blob, o, out.baseHash)) return false;
    uint16_t n = 0;
    if (!RdU16LE(blob, o, n)) return false;
    if (static_cast<size_t>(n) > maxRecords) return false;
    if (!W::Feasible(n, blob, o)) return false;                        // no count-driven balloons
    out.recs.resize(n);
    for (auto& r : out.recs) {
        if (!W::DeSave(blob, o, r)) return false;
    }
    if (op == kOpContentsExtract) {
        out.hasTail = true;
        uint8_t count = 0;
        if (!W::RdU8(blob, o, count)) return false;
        if (count == 0 || count > kMaxTokensPerWrite) return false;    // fail closed on a fake tail
        out.tokens.resize(count);
        for (auto& t : out.tokens) {
            if (!RdU64LE(blob, o, t)) return false;                    // truncated token
            if (t == 0) return false;                                  // token 0 is never valid
        }
    }
    // BOTH ops demand exact consumption: the stock b133 grammar has no trailing-bytes form
    // (the packer above is the only producer, and blob reassembly yields exact bytes), and a
    // fork tail that left residue would be indistinguishable from corruption. Fail closed.
    if (o != blob.size()) return false;
    return true;
}

}  // namespace coop::props::container_extract_wire
