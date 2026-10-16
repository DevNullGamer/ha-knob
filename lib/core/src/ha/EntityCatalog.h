#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "control/DomainActions.h"

// The entity picker list built from a get_states result (plan-step6.md §3.1, §3.5). Pure.
//
// peekEnvelope() looks at the first top-level scalars of a message (HA sends "id" first)
// so the session can recognise a get_states result without parsing it.
//
// EntityCatalogBuilder works through the whole result text in budgeted slices: a structural
// scan (util/JsonScan) finds the top-level "id" / "type" / "success" / "error" and each
// element of "result" (in any key order); each element is parsed alone with an ArduinoJson
// filter (entity_id, state, attributes.{kAttributeKeys}) and NestingLimit(kMaxNesting).
// An element that is too deep, malformed, has a kept string > 65535 bytes, a missing or
// over-long entity_id, or is larger than kMaxCatalogElementBytes is skipped (counted in
// skipped()); the others are kept. Truncated or structurally broken text fails the build.
// Only supported domains are listed, in HA order, at most kMaxCatalogEntries.
namespace core {
namespace ha {

constexpr size_t kMaxCatalogEntries = 1500;
constexpr size_t kMaxCatalogNameBytes = 96;          // UTF-8 safe cut
constexpr size_t kMaxCatalogStateBytes = 255;        // UTF-8 safe cut (HA caps states at 255)
constexpr size_t kMaxCatalogElementBytes = 256u * 1024u;  // larger elements are skipped
constexpr size_t kEnvelopePeekBytes = 1024;

struct EnvelopePeek {
    bool hasId = false;  // conclusive: a top-level "id" unsigned integer was found
    uint32_t id = 0;
    bool hasType = false;     // a top-level "type" string was found
    std::string_view type;    // raw (escapes not decoded), points into the scanned text
};

// Scans at most the first maxBytes of a message, top-level scalar members only; stops at
// the first object / array value (e.g. "result" first), at maxBytes, at the end of the
// object or on anything malformed. A member counts only if the ',' or '}' after its value
// lies within the limit (a value running into the limit may continue, so "id" stays
// unfound). Never reads past min(len, maxBytes).
EnvelopePeek peekEnvelope(const char *data, size_t len, size_t maxBytes = kEnvelopePeekBytes);

// One picker entry (a view into the EntityCatalog that returned it; valid while the
// catalog lives and is not moved from).
struct CatalogEntry {
    std::string_view id;
    std::string_view name;   // friendly_name, else the entity id; at most 96 bytes
    std::string_view state;  // "" if HA sent none
    const domain_actions::DomainSpec *domain = nullptr;
    uint8_t actionMask = 0;  // bit i: domain->actions[i] is available for this entity
    double stepDefault = 0;  // stepDefaultFor(actions()[0]); 0 for "none"

    // availableActions() in table order ("none" always present); [0] is the default.
    std::vector<const domain_actions::ActionSpec *> actions() const;
};

// Growable byte buffer on an ArduinoJson allocator (nullptr = malloc), so the catalog
// lives where the caller wants it (PSRAM on the device). Move-only.
class CatalogBuffer {
public:
    explicit CatalogBuffer(ArduinoJson::Allocator *allocator = nullptr) : allocator_(allocator) {}
    ~CatalogBuffer();
    CatalogBuffer(CatalogBuffer &&other) noexcept;
    CatalogBuffer &operator=(CatalogBuffer &&other) noexcept;
    CatalogBuffer(const CatalogBuffer &) = delete;
    CatalogBuffer &operator=(const CatalogBuffer &) = delete;

    bool append(const void *bytes, size_t n);  // false: out of memory (nothing appended)
    void clear();                              // frees the memory
    const char *data() const { return data_; }
    size_t size() const { return size_; }

private:
    void release();

    ArduinoJson::Allocator *allocator_;
    char *data_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = 0;
};

// The finished list. Strings are stored NUL-separated in one buffer and entries as
// fixed-size 16-byte records in another (1500 entries ~ 24 KB + text), both on the
// allocator.
class EntityCatalog {
public:
    explicit EntityCatalog(ArduinoJson::Allocator *allocator = nullptr);

    size_t size() const;
    bool empty() const { return size() == 0; }
    CatalogEntry entry(size_t index) const;  // index < size()

    uint32_t total() const { return total_; }      // elements in "result"
    uint32_t skipped() const { return skipped_; }  // elements that could not be used
    bool truncated() const { return truncated_; }  // supported entities beyond the cap

private:
    friend class EntityCatalogBuilder;

    struct Record {
        uint32_t textOffset;  // id '\0' name '\0' state '\0'
        uint8_t idLen;
        uint8_t nameLen;
        uint8_t stateLen;
        uint8_t domainIndex;  // into domain_actions::domains()
        uint8_t actionMask;
        double stepDefault;
    };

    bool add(std::string_view id, std::string_view name, std::string_view state,
             uint8_t domainIndex, uint8_t actionMask, double stepDefault);
    void clear();

    CatalogBuffer text_;
    CatalogBuffer records_;
    uint32_t total_ = 0;
    uint32_t skipped_ = 0;
    bool truncated_ = false;
};

class EntityCatalogBuilder {
public:
    enum class Status : uint8_t { Running, Done, Failed };

    // `data` must stay valid and unchanged until the build is Done or Failed.
    EntityCatalogBuilder(const char *data, size_t len, uint32_t expectedId,
                         ArduinoJson::Allocator *allocator = nullptr);

    // Processes whole units (one top-level member, or one "result" element with its
    // separator) until at least maxBytes (minimum 1) of text were consumed in this call,
    // so a step goes at most one element beyond its budget. The result is identical for
    // every budget. After Done / Failed it returns that status and does nothing.
    Status step(size_t maxBytes);

    Status status() const { return status_; }

    // Failed only: "states_unexpected" (wrong id / type, no result array, missing
    // "success", truncated or malformed text), "states_failed: <error.message>" (or
    // "states_failed" without a message or with one > 64 KB) for success:false,
    // "states_no_memory". A kept string > 65535 bytes in an element skips only that element.
    const std::string &error() const { return error_; }

    // The list (complete when Done; emptied on Failed).
    const EntityCatalog &entries() const { return catalog_; }
    uint32_t total() const { return catalog_.total(); }
    uint32_t skipped() const { return catalog_.skipped(); }
    bool truncated() const { return catalog_.truncated(); }
    size_t bytesProcessed() const { return pos_; }

    // Moves the list out (call once, when Done).
    EntityCatalog take() { return std::move(catalog_); }

private:
    enum class Phase : uint8_t { Start, FirstMember, NextMember, FirstElement, NextElement, Trailing };

    bool unit();  // one unit; false when the build ended (status_ set)
    bool member();
    bool element(size_t pos);
    bool envelopeValue(const char *key, size_t keyLen, size_t valueStart, size_t valueEnd);
    bool fail(const char *error);
    bool finish();

    const char *data_;
    size_t len_;
    uint32_t expectedId_;
    ArduinoJson::Allocator *allocator_;
    JsonDocument filter_;
    JsonDocument doc_;
    EntityCatalog catalog_;

    Status status_ = Status::Running;
    Phase phase_ = Phase::Start;
    size_t pos_ = 0;
    std::string error_;
    bool hasId_ = false;
    bool typeIsResult_ = false;
    bool hasType_ = false;
    bool hasSuccess_ = false;
    bool success_ = false;
    bool resultSeen_ = false;  // "result" was an array and its elements were scanned
    std::string errorMessage_;
};

// The 200 body of GET /api/ha/entities (plan-step6.md §3.5), compact JSON:
//   {"fetched_at_s":N,"total":T,"skipped":S,"truncated":false,"entities":[{"id":"light.k",
//    "name":"Kitchen","state":"on","actions":["brightness","none"],"step_default":5},..]}
//
// CatalogJsonWriter builds it in budgeted slices (R8: ~0.12 ms per entry on the device):
// the header is written by hand, then step(maxEntries) serialises entries one at a time
// with a reused JsonDocument on the allocator (no document for the whole list). The output
// is identical for every budget. The output std::string uses the default heap (on the
// device PSRAM through the SPIRAM malloc threshold, allocations >= 4 KB); it is reserved
// once with a lower bound of the final size computed from the records (exact except for
// escapes and step_default digits beyond the first + 4 bytes per entry). If an entry does
// not fit, the text is moved (usually at most once; escape-heavy names can need a few) into an exactly reserved larger string (sized from the
// remaining lower bound; never the std::string doubling), counted in regrowths().
class CatalogJsonWriter {
public:
    // `catalog` must stay valid and unchanged (not moved from) while the writer lives.
    CatalogJsonWriter(const EntityCatalog &catalog, uint32_t fetchedAtS,
                      ArduinoJson::Allocator *allocator = nullptr);
    CatalogJsonWriter(const CatalogJsonWriter &) = delete;
    CatalogJsonWriter &operator=(const CatalogJsonWriter &) = delete;

    // Writes up to maxEntries (minimum 1) entries; the step that writes the last entry (the
    // first step for an empty catalog) also closes the JSON. Returns done(); after that it
    // does nothing.
    bool step(size_t maxEntries);

    bool done() const { return done_; }
    size_t entriesWritten() const { return next_; }
    size_t regrowths() const { return regrowths_; }
    size_t capacity() const { return out_.capacity(); }

    // Moves the JSON out (call once, when done()).
    std::string take() { return std::move(out_); }

private:
    void ensureRoom(size_t extra);

    const EntityCatalog *catalog_;
    JsonDocument doc_;
    std::string item_;
    std::string out_;
    size_t next_ = 0;
    size_t lowerBoundLeft_ = 0;  // lower bound of the entries not written yet
    size_t regrowths_ = 0;
    bool done_ = false;
};

// One-shot wrapper around CatalogJsonWriter (same output).
std::string catalogToJson(const EntityCatalog &catalog, uint32_t fetchedAtS,
                          ArduinoJson::Allocator *allocator = nullptr);

}  // namespace ha
}  // namespace core
