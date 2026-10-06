#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "obe/engine/concepts.hpp"
#include "obe/journal/file.hpp"
#include "obe/journal/reader.hpp"
#include "obe/journal/replay.hpp"
#include "obe/journal/snapshot.hpp"
#include "obe/util/mapped_file.hpp"

// Recovery from files: the procedure a process runs when it starts and finds
// that an earlier one left a journal behind.
//
//   1. If there is a snapshot and it is sound, load it. The engine now holds
//      everything up to some journal record, and the snapshot says which.
//      If there is one and it is NOT sound, ignore it and say so. The journal
//      is the record; a snapshot is only a way of not reading all of it.
//   2. Replay the journal from that record on (or from the first).
//   3. If the journal ends in a torn tail, cut the file back to the last whole
//      record, so that what is appended next follows a good record.
//   4. Carry on appending, numbering from where the replay stopped.
//
// Steps 1 to 3 are recover(). Step 4 is the caller's: open the file with
// JournalFile and give a JournalWriter the next sequence number, with a file
// header only if the file is empty. Recovered::next_sequence() and
// Recovered::file_is_empty() are those two facts.
//
// ONE RULE FOR WHOEVER TAKES SNAPSHOTS
//
// A snapshot that says "I include everything before record N" is only usable
// together with a journal that still has record N-1, or with one that goes on
// from N. So: make the journal durable (flush and sync) BEFORE writing the
// snapshot. If the snapshot were written first and the machine lost power,
// the journal on disk could end before N, and the pair would describe two
// different histories. recover() notices that case (BadSequence) and does not
// guess.

namespace obe::journal {

struct Recovered {
    bool snapshot_found = false;          // a snapshot file was there
    bool snapshot_used = false;           // and it was loaded into the engine
    std::uint64_t snapshot_sequence = 0;  // the first record the snapshot does not include
    std::size_t snapshot_orders = 0;

    bool journal_found = false;
    std::size_t journal_bytes = 0;  // the size of the journal file as it was found
    std::size_t cut = 0;            // bytes of torn tail removed from it
    ReplayResult replay;

    // The engine holds everything the files can vouch for, and the journal
    // can be carried on.
    [[nodiscard]] constexpr bool ok() const noexcept { return replay.usable(); }
    // The number to give the next record.
    [[nodiscard]] constexpr std::uint64_t next_sequence() const noexcept {
        return replay.next_sequence;
    }
    // Whether the journal file has nothing in it, so that a writer carrying
    // on must begin with a file header.
    [[nodiscard]] constexpr bool file_is_empty() const noexcept { return replay.good_bytes == 0; }
};

// Brings a fresh `engine` to the state the files describe.
//
// `snapshot_path` may be empty: then only the journal is used. A journal file
// that does not exist is treated as an empty one, which is what a first start
// looks like.
//
// With `repair`, a torn tail is cut off the journal file (and the cut synced).
// Without it nothing on disk is changed, which is what a tool that only
// inspects wants.
//
// Throws std::runtime_error if a file is there and cannot be read or cut.
template <engine::EngineLike Engine>
Recovered recover(const std::string& journal_path, const std::string& snapshot_path, Engine& engine,
                  bool repair = true) {
    Recovered out;
    std::uint64_t from_seq = 1;

    if (!snapshot_path.empty() && std::filesystem::exists(snapshot_path)) {
        out.snapshot_found = true;
        if constexpr (engine::Restorable<Engine>) {
            const util::MappedFile file(snapshot_path, util::MappedFile::Advice::None);
            const std::optional<EngineState> state = decode(file.bytes());
            if (state && restore(*state, engine)) {
                out.snapshot_used = true;
                out.snapshot_sequence = state->next_sequence;
                out.snapshot_orders = state->orders.size();
                from_seq = state->next_sequence;
            }
        }
    }

    if (!std::filesystem::exists(journal_path)) {
        out.replay = replay({}, engine, from_seq);
        return out;
    }
    out.journal_found = true;
    {
        // The mapping must be gone before the file is cut: touching a mapped
        // page beyond the new end of a file is a bus error.
        const util::MappedFile file(journal_path);
        out.journal_bytes = file.size();
        out.replay = replay(file.bytes(), engine, from_seq);
    }
    if (repair && out.replay.status == ReadStatus::TornTail &&
        out.replay.good_bytes < out.journal_bytes) {
        JournalFile file(journal_path);
        file.truncate(out.replay.good_bytes);
        file.sync();
        out.cut = out.journal_bytes - out.replay.good_bytes;
    }
    return out;
}

}  // namespace obe::journal
