#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace cachedb
{

  // The in-memory half of the storage engine: every write lands here first and
  // every read consults it before touching a disk.
  //
  // Sorted, and that is not a detail. A flush turns this into an SSTable, and
  // an SSTable is sorted by key -- keeping the order here means a flush walks
  // the table once, in order, with no sort step and no second copy of a
  // structure that is deliberately sized to be large. PROJECT.md 6.3.
  class Memtable
  {
  public:
    struct Entry
    {
      std::string value;
      // A delete records a tombstone rather than erasing the row, because the
      // key may still exist in an older SSTable this table knows nothing
      // about, and the marker is the only thing that will hide it.
      bool tombstone = false;

      // Wall-clock milliseconds at which this entry stops being visible, or 0
      // for "never". PROJECT.md 6.8.
      //
      // Stored raw and never interpreted here. Deciding whether a stamp is in
      // the past means knowing what "now" is, and letting three layers each
      // ask the clock separately is how a key expires in the memtable but not
      // in the SSTable it is hiding. Store owns that comparison, for itself
      // and for the tables below it.
      int64_t expires_at_ms = 0;
    };

    // Three answers, not two, and the distinction is the whole reason this
    // returns a pointer:
    //
    //   nullptr                  not in this memtable -- keep looking below
    //   entry, tombstone = false the value, and the search is over
    //   entry, tombstone = true  deleted -- the search is over, answer absent
    //
    // Collapsing the first and third into one "absent" is the classic LSM
    // bug: the read falls through to an older file, finds the key the
    // tombstone was hiding, and resurrects data the client deleted.
    //
    // The pointer stays valid until this key is written again. std::map holds
    // each node at a stable address, the same property that lets a value view
    // outlive the lookup that produced it.
    const Entry *find(std::string_view key) const;

    // expires_at_ms = 0 means no expiry, and a plain SET passes 0 on purpose:
    // real Redis drops an existing TTL when a key is overwritten without one,
    // so carrying the old stamp forward would be the wrong behaviour rather
    // than a conservative one.
    void set(std::string_view key, std::string_view value,
             int64_t expires_at_ms = 0);

    // Reports whether the key was live beforehand, which is DEL's reply count.
    bool del(std::string_view key);

    // Turns expired entries into tombstones, examining at most `budget`
    // entries and resuming next time where this one stopped. Returns how many
    // were swept. PROJECT.md 6.8's active half.
    //
    // `now` is passed in rather than read here, which keeps this file free of
    // clocks: Store does the judging, for this table and for the SSTables
    // underneath it, so the two cannot disagree about the time.
    //
    // A cursor rather than random sampling. Redis samples because its hash
    // table can pick a random slot in O(1); reaching a random element of a
    // std::map means walking to it, so sampling here would cost a walk per
    // sample and still visit some keys never. A cursor bounds the work per
    // tick and guarantees every key is looked at eventually.
    size_t sweep_expired(int64_t now, size_t budget);

    // Walks every entry in key order, tombstones included -- a flush has to
    // write those too, or the SSTable it produces would silently drop the
    // deletes and the keys would come back.
    //
    // A callback rather than begin()/end() so that no caller names the backing
    // container. PROJECT.md 6.3 wants a skip list measured against std::map;
    // with this shape that swap touches this file and nothing else.
    void for_each(
        const std::function<void(std::string_view, const Entry &)> &fn) const;

    bool empty() const { return entries_.empty(); }

    // Entries including tombstones: what a flush would write.
    size_t entry_count() const { return entries_.size(); }

    // Live keys only, which is what a client asking for a key count means.
    //
    // Counts an expired entry that the sweep has not reached yet, because
    // knowing otherwise would mean reading the clock here. It is a status
    // line, not an answer a client acts on.
    size_t live_count() const { return live_count_; }

    // Approximate bytes held, used to decide when to flush. Accounts for
    // allocator overhead, not just key and value lengths -- see memtable.cpp,
    // where the difference was measured against real RSS.
    size_t bytes() const { return bytes_; }

    void clear();

  private:
    // std::less<> instead of the default makes the comparator transparent,
    // which lets find() accept a string_view as-is. Without it every lookup
    // would construct a temporary std::string from the key: an allocation on
    // the hot path of every single GET.
    std::map<std::string, Entry, std::less<>> entries_;
    // Where the next sweep resumes. A key and not an iterator: clear() would
    // dangle an iterator, and a key simply stops matching anything, at which
    // point lower_bound starts the walk again from wherever is nearest.
    std::string sweep_cursor_;
    size_t live_count_ = 0;
    size_t bytes_ = 0;
  };

} // namespace cachedb
