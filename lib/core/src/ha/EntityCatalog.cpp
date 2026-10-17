#include "ha/EntityCatalog.h"

#include <cstdlib>
#include <cstring>
#include <utility>

#include "config/ConfigModel.h"
#include "ha/EntityAttrs.h"
#include "ha/HaProtocol.h"
#include "util/JsonScan.h"

namespace core {
namespace ha {

namespace js = util::json_scan;
namespace da = domain_actions;

namespace {

constexpr size_t kMinBufferCapacity = 4096;  // also keeps malloc fallbacks out of internal RAM

// ArduinoJson's string length limit (ARDUINOJSON_STRING_LENGTH_SIZE 2). A kept string beyond
// it is reported as NoMemory; that is only possible if the parsed text is longer than this.
constexpr size_t kMaxKeptStringBytes = 65535;

JsonDocument makeDoc(ArduinoJson::Allocator *allocator) {
    return allocator != nullptr ? JsonDocument(allocator) : JsonDocument();
}

// Longest prefix of s with at most maxBytes bytes that does not split a UTF-8 sequence.
std::string_view cutUtf8(std::string_view s, size_t maxBytes) {
    if (s.size() <= maxBytes) {
        return s;
    }
    size_t n = maxBytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0u) == 0x80u) {
        --n;
    }
    return s.substr(0, n);
}

std::string_view viewOf(JsonVariantConst v) {
    if (!v.is<const char *>()) {
        return std::string_view();
    }
    const JsonString s = v.as<JsonString>();
    return std::string_view(s.c_str(), s.size());
}

}  // namespace

// ---- peekEnvelope -------------------------------------------------------------------------

EnvelopePeek peekEnvelope(const char *data, size_t len, size_t maxBytes) {
    EnvelopePeek peek;
    if (data == nullptr) {
        return peek;
    }
    const size_t limit = len < maxBytes ? len : maxBytes;
    size_t p = js::skipWhitespace(data, limit, 0);
    if (p >= limit || data[p] != '{') {
        return peek;
    }
    p = js::skipWhitespace(data, limit, p + 1);
    while (p < limit && data[p] == '"') {
        size_t keyEnd = 0;
        if (js::scanString(data, limit, p, keyEnd) != js::ScanStatus::Ok) {
            break;
        }
        const char *key = data + p + 1;
        const size_t keyLen = keyEnd - p - 2;
        p = js::skipWhitespace(data, limit, keyEnd);
        if (p >= limit || data[p] != ':') {
            break;
        }
        p = js::skipWhitespace(data, limit, p + 1);
        if (p >= limit || data[p] == '{' || data[p] == '[') {
            break;  // only top-level scalars are looked at
        }
        size_t valueEnd = 0;
        if (js::skipValue(data, limit, p, valueEnd) != js::ScanStatus::Ok) {
            break;
        }
        // A value counts only if its delimiter lies within the limit: a scalar that runs
        // into the limit may continue ("id":7 cut from "id":70).
        const size_t next = js::skipWhitespace(data, limit, valueEnd);
        if (next >= limit || (data[next] != ',' && data[next] != '}')) {
            break;
        }
        if (js::equals(key, keyLen, "id") && !peek.hasId) {
            peek.hasId = js::parseUint32(data + p, valueEnd - p, peek.id);
        } else if (js::equals(key, keyLen, "type") && !peek.hasType && data[p] == '"') {
            peek.hasType = true;
            peek.type = std::string_view(data + p + 1, valueEnd - p - 2);
        }
        if ((peek.hasId && peek.hasType) || data[next] == '}') {
            break;
        }
        p = js::skipWhitespace(data, limit, next + 1);
    }
    return peek;
}

// ---- CatalogBuffer ------------------------------------------------------------------------

CatalogBuffer::~CatalogBuffer() { release(); }

CatalogBuffer::CatalogBuffer(CatalogBuffer &&other) noexcept
    : allocator_(other.allocator_), data_(other.data_), size_(other.size_), capacity_(other.capacity_) {
    other.data_ = nullptr;
    other.size_ = 0;
    other.capacity_ = 0;
}

CatalogBuffer &CatalogBuffer::operator=(CatalogBuffer &&other) noexcept {
    if (this != &other) {
        release();
        allocator_ = other.allocator_;
        data_ = other.data_;
        size_ = other.size_;
        capacity_ = other.capacity_;
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
    }
    return *this;
}

bool CatalogBuffer::append(const void *bytes, size_t n) {
    if (n == 0) {
        return true;
    }
    if (n > SIZE_MAX - size_) {
        return false;
    }
    if (size_ + n > capacity_) {
        size_t capacity = capacity_ < kMinBufferCapacity ? kMinBufferCapacity : capacity_;
        while (capacity < size_ + n) {
            capacity = capacity > SIZE_MAX / 2 ? size_ + n : capacity * 2;
        }
        void *p = nullptr;
        if (allocator_ != nullptr) {
            p = data_ == nullptr ? allocator_->allocate(capacity) : allocator_->reallocate(data_, capacity);
        } else {
            p = std::realloc(data_, capacity);
        }
        if (p == nullptr) {
            return false;  // data_ is still valid
        }
        data_ = static_cast<char *>(p);
        capacity_ = capacity;
    }
    std::memcpy(data_ + size_, bytes, n);
    size_ += n;
    return true;
}

void CatalogBuffer::clear() { release(); }

void CatalogBuffer::release() {
    if (data_ != nullptr) {
        if (allocator_ != nullptr) {
            allocator_->deallocate(data_);
        } else {
            std::free(data_);
        }
    }
    data_ = nullptr;
    size_ = 0;
    capacity_ = 0;
}

// ---- EntityCatalog ------------------------------------------------------------------------

std::vector<const da::ActionSpec *> CatalogEntry::actions() const {
    std::vector<const da::ActionSpec *> out;
    if (domain == nullptr) {
        return out;
    }
    for (size_t i = 0; i < domain->actionCount && i < 8; ++i) {
        if (actionMask & (1u << i)) {
            out.push_back(&domain->actions[i]);
        }
    }
    return out;
}

EntityCatalog::EntityCatalog(ArduinoJson::Allocator *allocator) : text_(allocator), records_(allocator) {}

size_t EntityCatalog::size() const { return records_.size() / sizeof(Record); }

CatalogEntry EntityCatalog::entry(size_t index) const {
    Record r;
    std::memcpy(&r, records_.data() + index * sizeof(Record), sizeof(Record));
    size_t count = 0;
    const da::DomainSpec *table = da::domains(count);
    const char *text = text_.data() + r.textOffset;
    CatalogEntry e;
    e.id = std::string_view(text, r.idLen);
    e.name = std::string_view(text + r.idLen + 1, r.nameLen);
    e.state = std::string_view(text + r.idLen + 1 + r.nameLen + 1, r.stateLen);
    e.domain = r.domainIndex < count ? &table[r.domainIndex] : nullptr;
    e.actionMask = r.actionMask;
    e.stepDefault = r.stepDefault;
    return e;
}

bool EntityCatalog::add(std::string_view id, std::string_view name, std::string_view state,
                        uint8_t domainIndex, uint8_t actionMask, double stepDefault) {
    Record r;
    std::memset(&r, 0, sizeof(r));  // padding bytes too
    r.textOffset = static_cast<uint32_t>(text_.size());
    r.idLen = static_cast<uint8_t>(id.size());
    r.nameLen = static_cast<uint8_t>(name.size());
    r.stateLen = static_cast<uint8_t>(state.size());
    r.domainIndex = domainIndex;
    r.actionMask = actionMask;
    r.stepDefault = stepDefault;
    // on failure the build fails and the whole catalog is cleared
    const char nul = '\0';
    return text_.append(id.data(), id.size()) && text_.append(&nul, 1) &&
           text_.append(name.data(), name.size()) && text_.append(&nul, 1) &&
           text_.append(state.data(), state.size()) && text_.append(&nul, 1) &&
           records_.append(&r, sizeof(r));
}

void EntityCatalog::clear() {
    text_.clear();
    records_.clear();
    total_ = 0;
    skipped_ = 0;
    truncated_ = false;
}

// ---- EntityCatalogBuilder -----------------------------------------------------------------

EntityCatalogBuilder::EntityCatalogBuilder(const char *data, size_t len, uint32_t expectedId,
                                           ArduinoJson::Allocator *allocator)
    : data_(data),
      len_(data != nullptr ? len : 0),
      expectedId_(expectedId),
      allocator_(allocator),
      filter_(makeDoc(allocator)),
      doc_(makeDoc(allocator)),
      catalog_(allocator) {
    filter_["entity_id"] = true;
    filter_["state"] = true;
    addAttributeFilter(filter_["attributes"].to<JsonObject>());
}

EntityCatalogBuilder::Status EntityCatalogBuilder::step(size_t maxBytes, size_t maxElements) {
    if (status_ != Status::Running) {
        return status_;
    }
    const size_t budget = maxBytes == 0 ? 1 : maxBytes;
    const size_t elementBudget = maxElements == 0 ? 1 : maxElements;
    const size_t start = pos_;
    const uint32_t startElements = catalog_.total_;  // counts every element unit
    while (unit()) {
        if (pos_ - start >= budget || catalog_.total_ - startElements >= elementBudget) {
            break;
        }
    }
    return status_;
}

bool EntityCatalogBuilder::unit() {
    switch (phase_) {
        case Phase::Start: {
            const size_t p = js::skipWhitespace(data_, len_, 0);
            if (p >= len_ || data_[p] != '{') {
                return fail("states_unexpected");
            }
            pos_ = p + 1;
            phase_ = Phase::FirstMember;
            return true;
        }
        case Phase::FirstMember:
        case Phase::NextMember:
            return member();
        case Phase::FirstElement:
        case Phase::NextElement: {
            size_t p = js::skipWhitespace(data_, len_, pos_);
            if (p >= len_) {
                return fail("states_unexpected");
            }
            if (data_[p] == ']') {
                pos_ = p + 1;
                phase_ = Phase::NextMember;
                return true;
            }
            if (phase_ == Phase::NextElement) {
                if (data_[p] != ',') {
                    return fail("states_unexpected");
                }
                p = js::skipWhitespace(data_, len_, p + 1);
            }
            phase_ = Phase::NextElement;
            return element(p);
        }
        case Phase::Trailing:
        default: {
            const size_t p = js::skipWhitespace(data_, len_, pos_);
            pos_ = p;
            if (p != len_) {
                return fail("states_unexpected");
            }
            return finish();
        }
    }
}

bool EntityCatalogBuilder::member() {
    size_t p = js::skipWhitespace(data_, len_, pos_);
    if (p >= len_) {
        return fail("states_unexpected");
    }
    if (data_[p] == '}') {
        pos_ = p + 1;
        phase_ = Phase::Trailing;
        return true;
    }
    if (phase_ == Phase::NextMember) {
        if (data_[p] != ',') {
            return fail("states_unexpected");
        }
        p = js::skipWhitespace(data_, len_, p + 1);
    }
    size_t keyEnd = 0;
    if (js::scanString(data_, len_, p, keyEnd) != js::ScanStatus::Ok) {
        return fail("states_unexpected");
    }
    const char *key = data_ + p + 1;
    const size_t keyLen = keyEnd - p - 2;
    p = js::skipWhitespace(data_, len_, keyEnd);
    if (p >= len_ || data_[p] != ':') {
        return fail("states_unexpected");
    }
    p = js::skipWhitespace(data_, len_, p + 1);
    if (p >= len_) {
        return fail("states_unexpected");
    }
    if (!resultSeen_ && data_[p] == '[' && js::equals(key, keyLen, "result")) {
        resultSeen_ = true;
        pos_ = p + 1;
        phase_ = Phase::FirstElement;
        return true;
    }
    size_t valueEnd = 0;
    if (js::skipValue(data_, len_, p, valueEnd) != js::ScanStatus::Ok) {
        return fail("states_unexpected");
    }
    if (!envelopeValue(key, keyLen, p, valueEnd)) {
        return false;
    }
    pos_ = valueEnd;
    phase_ = Phase::NextMember;
    return true;
}

bool EntityCatalogBuilder::envelopeValue(const char *key, size_t keyLen, size_t valueStart,
                                         size_t valueEnd) {
    const char *value = data_ + valueStart;
    const size_t valueLen = valueEnd - valueStart;
    if (js::equals(key, keyLen, "id")) {
        uint32_t id = 0;
        if (!js::parseUint32(value, valueLen, id) || id != expectedId_) {
            return fail("states_unexpected");
        }
        hasId_ = true;
    } else if (js::equals(key, keyLen, "type")) {
        // raw compare: "result" never needs escapes
        if (valueLen < 2 || value[0] != '"' || !js::equals(value + 1, valueLen - 2, "result")) {
            return fail("states_unexpected");
        }
        hasType_ = true;
    } else if (js::equals(key, keyLen, "success")) {
        if (js::equals(value, valueLen, "true")) {
            success_ = true;
        } else if (js::equals(value, valueLen, "false")) {
            success_ = false;
        } else {
            return fail("states_unexpected");
        }
        hasSuccess_ = true;
    } else if (js::equals(key, keyLen, "error")) {
        JsonDocument filter = makeDoc(allocator_);
        filter["message"] = true;
        doc_.clear();
        const DeserializationError err =
            deserializeJson(doc_, value, valueLen, DeserializationOption::Filter(filter),
                            DeserializationOption::NestingLimit(kMaxNesting));
        if (err == DeserializationError::NoMemory && valueLen <= kMaxKeptStringBytes) {
            return fail("states_no_memory");
        }
        if (!err) {
            errorMessage_ = std::string(viewOf(doc_["message"]));
        }  // else (also a message > 64 KB): no message, plain "states_failed"
        doc_.clear();
    }
    return true;
}

bool EntityCatalogBuilder::element(size_t start) {
    size_t end = 0;
    if (start >= len_ || js::skipValue(data_, len_, start, end) != js::ScanStatus::Ok) {
        return fail("states_unexpected");
    }
    pos_ = end;
    ++catalog_.total_;
    const size_t n = end - start;
    if (n > kMaxCatalogElementBytes) {
        ++catalog_.skipped_;
        return true;
    }
    doc_.clear();
    const DeserializationError err =
        deserializeJson(doc_, data_ + start, n, DeserializationOption::Filter(filter_),
                        DeserializationOption::NestingLimit(kMaxNesting));
    if (err == DeserializationError::NoMemory && n <= kMaxKeptStringBytes) {
        return fail("states_no_memory");
    }
    if (err || !doc_.is<JsonObjectConst>()) {  // incl. a kept string > 64 KB (NoMemory)
        ++catalog_.skipped_;
        return true;
    }
    const std::string_view id = viewOf(doc_["entity_id"]);
    if (id.empty() || id.size() > config::kMaxEntityIdLen) {
        ++catalog_.skipped_;
        return true;
    }
    const da::DomainSpec *domain = da::findDomain(da::domainOf(id));
    if (domain == nullptr) {
        return true;  // unsupported domain: not listed, not an error
    }
    if (catalog_.size() >= kMaxCatalogEntries) {
        catalog_.truncated_ = true;
        return true;
    }
    EntityAttrs attrs;
    applyAttrs(doc_["attributes"].as<JsonObjectConst>(), attrs, AttrMode::Replace);
    const std::vector<const da::ActionSpec *> actions = da::availableActions(*domain, attrs);
    uint8_t mask = 0;
    for (const da::ActionSpec *action : actions) {
        mask |= static_cast<uint8_t>(1u << (action - domain->actions));
    }
    const double stepDefault = actions.empty() ? 0 : da::stepDefaultFor(*actions.front(), attrs);
    const std::string_view name =
        cutUtf8(attrs.friendlyName.empty() ? id : std::string_view(attrs.friendlyName),
                kMaxCatalogNameBytes);
    const std::string_view state = cutUtf8(viewOf(doc_["state"]), kMaxCatalogStateBytes);
    size_t count = 0;
    const uint8_t domainIndex = static_cast<uint8_t>(domain - da::domains(count));
    if (!catalog_.add(id, name, state, domainIndex, mask, stepDefault)) {
        return fail("states_no_memory");
    }
    return true;
}

bool EntityCatalogBuilder::fail(const char *error) {
    catalog_.clear();
    doc_.clear();
    error_ = error;
    status_ = Status::Failed;
    return false;
}

bool EntityCatalogBuilder::finish() {
    if (!hasId_ || !hasType_ || !hasSuccess_) {
        return fail("states_unexpected");
    }
    if (!success_) {
        const std::string msg = errorMessage_;
        fail("states_failed");
        if (!msg.empty()) {
            error_ += ": " + msg;
        }
        return false;
    }
    if (!resultSeen_) {
        return fail("states_unexpected");
    }
    doc_.clear();
    status_ = Status::Done;
    return false;
}

// ---- CatalogJsonWriter --------------------------------------------------------------------

namespace {

constexpr size_t kJsonExtraPerEntry = 4;   // reserve beyond the lower bound (step_default "0.05")
constexpr size_t kJsonRegrowSlack = 1024;  // extra room when the text has to be moved
constexpr char kJsonClose[] = "]}";
constexpr size_t kJsonCloseLen = sizeof(kJsonClose) - 1;

// Fixed characters of one entry: {"id":"","name":"","state":"","actions":[],"step_default":}
constexpr size_t kJsonEntryFixed = 59;

// Bytes entry `index` takes at least in the output (with its leading ','): unescaped
// strings and a one-character step_default. Mirrors the serialisation in step().
size_t entryLowerBound(const CatalogEntry &e, size_t index) {
    size_t n = kJsonEntryFixed + e.id.size() + e.name.size() + e.state.size() + 1;
    if (index > 0) {
        ++n;
    }
    if (e.domain != nullptr) {
        size_t actions = 0;
        for (size_t i = 0; i < e.domain->actionCount && i < 8; ++i) {
            if (e.actionMask & (1u << i)) {
                n += std::strlen(e.domain->actions[i].name) + 2 + (actions > 0 ? 1 : 0);
                ++actions;
            }
        }
    }
    return n;
}

}  // namespace

CatalogJsonWriter::CatalogJsonWriter(const EntityCatalog &catalog, uint32_t fetchedAtS,
                                     ArduinoJson::Allocator *allocator)
    : catalog_(&catalog), doc_(makeDoc(allocator)) {
    std::string header = "{\"fetched_at_s\":";
    header += std::to_string(fetchedAtS);
    header += ",\"total\":";
    header += std::to_string(catalog.total());
    header += ",\"skipped\":";
    header += std::to_string(catalog.skipped());
    header += ",\"truncated\":";
    header += catalog.truncated() ? "true" : "false";
    header += ",\"entities\":[";
    const size_t count = catalog.size();
    for (size_t i = 0; i < count; ++i) {
        lowerBoundLeft_ += entryLowerBound(catalog.entry(i), i);
    }
    // reserve() on an empty string allocates exactly this much (no doubling)
    out_.reserve(header.size() + lowerBoundLeft_ + count * kJsonExtraPerEntry + kJsonCloseLen);
    out_ += header;
}

void CatalogJsonWriter::ensureRoom(size_t extra) {
    const size_t needed = out_.size() + extra;
    if (needed <= out_.capacity()) {
        return;
    }
    const size_t entriesLeft = catalog_->size() - next_ - 1;  // after the one being added
    std::string bigger;
    bigger.reserve(needed + lowerBoundLeft_ + entriesLeft * kJsonExtraPerEntry + kJsonRegrowSlack);
    bigger += out_;
    out_.swap(bigger);
    ++regrowths_;
}

bool CatalogJsonWriter::step(size_t maxEntries) {
    if (done_) {
        return true;
    }
    const size_t count = catalog_->size();
    const size_t budget = maxEntries == 0 ? 1 : maxEntries;
    for (size_t written = 0; written < budget && next_ < count; ++written) {
        const CatalogEntry e = catalog_->entry(next_);
        doc_.clear();
        doc_["id"] = JsonString(e.id.data(), e.id.size());
        doc_["name"] = JsonString(e.name.data(), e.name.size());
        doc_["state"] = JsonString(e.state.data(), e.state.size());
        JsonArray actions = doc_["actions"].to<JsonArray>();
        if (e.domain != nullptr) {  // as CatalogEntry::actions(), without the vector
            for (size_t i = 0; i < e.domain->actionCount && i < 8; ++i) {
                if (e.actionMask & (1u << i)) {
                    const char *name = e.domain->actions[i].name;
                    actions.add(JsonString(name, std::strlen(name), true));
                }
            }
        }
        doc_["step_default"] = e.stepDefault;
        item_.clear();
        serializeJson(doc_, item_);
        const size_t bound = entryLowerBound(e, next_);
        lowerBoundLeft_ = lowerBoundLeft_ > bound ? lowerBoundLeft_ - bound : 0;
        ensureRoom((next_ > 0 ? 1 : 0) + item_.size() + kJsonCloseLen);  // the close always fits
        if (next_ > 0) {
            out_ += ',';
        }
        out_ += item_;
        ++next_;
    }
    if (next_ == count) {
        out_ += kJsonClose;
        done_ = true;
        doc_.clear();
    }
    return done_;
}

std::string catalogToJson(const EntityCatalog &catalog, uint32_t fetchedAtS,
                          ArduinoJson::Allocator *allocator) {
    CatalogJsonWriter writer(catalog, fetchedAtS, allocator);
    while (!writer.step(SIZE_MAX)) {
    }
    return writer.take();
}

}  // namespace ha
}  // namespace core
