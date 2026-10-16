#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

#include "ha/EntityAttrs.h"
#include "util/StdAllocator.h"

// Live state of the subscribed entities (plan-step6.md §3.3), fed with the "event" object
// of subscribe_entities messages in HA's compressed format:
//   {"a":{"light.x":{"s":"on","a":{..},"c":..,"lc":..}}}         added / initial state
//   {"c":{"light.x":{"+":{"s":"off","a":{..},"lc":..},"-":{"a":["brightness"]}}}}  changed
//   {"r":["light.x"]}                                             removed
// Pure. The session decides which events belong to the current subscription; the store
// applies whatever it is given. At most kMaxEntries entities (new ones beyond are ignored).
// The map nodes, ids, states and names live on the allocator given to the constructor (on
// the device PSRAM, so 24-64 entities do not take internal RAM; nullptr = default heap).
namespace core {
namespace ha {

struct EntityState {
    EntityState() = default;
    explicit EntityState(ArduinoJson::Allocator *allocator)
        : state(util::StdAllocator<char>(allocator)), attrs(allocator) {}

    util::AllocString state;  // "" if HA sent none / not a string
    EntityAttrs attrs;

    bool operator==(const EntityState &o) const { return state == o.state && attrs == o.attrs; }
    bool operator!=(const EntityState &o) const { return !(*this == o); }
};

class EntityStore {
public:
    static constexpr size_t kMaxEntries = 64;

    explicit EntityStore(ArduinoJson::Allocator *allocator = nullptr);

    // Applies one event object, in the order "a", "c", "r":
    //   a: insert or replace (attributes Replace); non-object entries are ignored;
    //   c: unknown ids ignored; "-.a" resets the listed keys, then "+.s" sets the state
    //      and "+.a" merges; context ("c", may be a string or an object), "lc", "lu" ignored;
    //   r: remove.
    // A non-object event changes nothing.
    void apply(JsonVariantConst event);

    const EntityState *find(std::string_view entityId) const;
    size_t size() const { return entities_.size(); }

    // +1 for every entity that was actually inserted, modified or removed (an update that
    // leaves an entity as it was does not count), +1 when snapshotSeen() turns true (also
    // for an empty "a"), and +1 on a clear() that drops entities or the snapshot flag.
    uint32_t version() const { return version_; }

    // True once an "a" object arrived since the last clear() (the subscription's snapshot).
    bool snapshotSeen() const { return snapshotSeen_; }

    // Drops all entities and the snapshot flag (new subscription or new HA instance).
    void clear();

private:
    void applyAdded(JsonObjectConst added);
    void applyChanged(JsonObjectConst changed);
    void applyRemoved(JsonArrayConst removed);

    using Map = std::map<util::AllocString, EntityState, std::less<>,
                         util::StdAllocator<std::pair<const util::AllocString, EntityState>>>;

    ArduinoJson::Allocator *allocator_;
    Map entities_;
    uint32_t version_ = 0;
    bool snapshotSeen_ = false;
};

}  // namespace ha
}  // namespace core
