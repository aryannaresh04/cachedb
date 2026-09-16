#include "compaction.h"

#include <queue>
#include <string_view>

namespace cachedb {
namespace {

// One input's position in the merge. The cursor itself is not stored here:
// it lives in a vector alongside, because a priority_queue copies its
// elements and a cursor is not something to copy per comparison.
struct Head {
  size_t input = 0;  // index into inputs / cursors
  std::string key;   // the cursor's current key, copied
};

// Smallest key first; ties broken by input order, which is newest first. So
// the first time a key comes out of the heap it is the newest version of it,
// and every later one is superseded.
//
// Greater-than, because priority_queue hands back its *largest* element.
struct Order {
  bool operator()(const Head& a, const Head& b) const {
    if (a.key != b.key) return a.key > b.key;
    return a.input > b.input;
  }
};

}  // namespace

CompactionResult compact(const std::vector<const Sstable*>& inputs,
                         const std::string& out_path, int bits_per_key,
                         bool drop_obsolete, int64_t now_ms) {
  CompactionResult result;

  std::vector<Sstable::Cursor> cursors;
  cursors.reserve(inputs.size());
  for (const Sstable* table : inputs) cursors.push_back(table->cursor());

  // A heap and not a scan across the inputs. With four tables the two cost
  // about the same and the scan is simpler; the heap is here because the
  // number of inputs is a tuning decision, and a scan turns k from a constant
  // into a factor on every single entry.
  std::priority_queue<Head, std::vector<Head>, Order> heap;
  for (size_t i = 0; i < cursors.size(); ++i) {
    if (cursors[i].failed()) return result;
    if (cursors[i].valid()) heap.push({i, std::string(cursors[i].key())});
  }

  SstableWriter writer(out_path, bits_per_key);
  std::string previous;
  bool have_previous = false;

  while (!heap.empty()) {
    const Head head = heap.top();
    heap.pop();
    Sstable::Cursor& cursor = cursors[head.input];

    // Every key after the first with this name comes from an older table and
    // is already answered. This is where the read path's "the first table
    // with anything to say settles it" becomes a physical deletion.
    const bool superseded = have_previous && previous == head.key;
    if (superseded) {
      ++result.dropped;
    } else {
      const bool expired =
          cursor.expires_at_ms() != 0 && cursor.expires_at_ms() <= now_ms;
      // A tombstone and an expired entry are the same thing here: a row that
      // exists only to hide something older. Once nothing older is left, both
      // are dead weight -- and until then, both must be written out.
      if (drop_obsolete && (cursor.tombstone() || expired)) {
        ++result.dropped;
      } else {
        writer.add(cursor.key(), cursor.value(), cursor.tombstone(),
                   cursor.expires_at_ms());
        ++result.written;
      }
      previous = head.key;
      have_previous = true;
    }

    cursor.next();
    if (cursor.failed()) {
      // Refusing here rather than finishing a short table. A truncated merge
      // that replaced its inputs would lose every key past the error, and the
      // result would look like a perfectly good SSTable.
      return result;
    }
    if (cursor.valid()) heap.push({head.input, std::string(cursor.key())});
  }

  if (!writer.finish()) return result;
  result.ok = true;
  return result;
}

}  // namespace cachedb
