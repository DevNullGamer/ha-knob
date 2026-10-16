#include "ha/EntityStore.h"

namespace core {
namespace ha {

namespace {

std::string_view keyOf(const JsonPairConst &kv) {
    return std::string_view(kv.key().c_str(), kv.key().size());
}

}  // namespace

void EntityStore::apply(JsonVariantConst event) {
    JsonObjectConst obj = event.as<JsonObjectConst>();
    if (obj.isNull()) {
        return;
    }
    JsonVariantConst added = obj["a"];
    if (added.is<JsonObjectConst>()) {
        if (!snapshotSeen_) {
            snapshotSeen_ = true;
            ++version_;  // also an empty snapshot ({"a":{}}) is news for the consumer
        }
        applyAdded(added.as<JsonObjectConst>());
    }
    applyChanged(obj["c"].as<JsonObjectConst>());
    applyRemoved(obj["r"].as<JsonArrayConst>());
}

void EntityStore::applyAdded(JsonObjectConst added) {
    for (JsonPairConst kv : added) {
        const std::string_view id = keyOf(kv);
        JsonObjectConst entry = kv.value().as<JsonObjectConst>();
        if (entry.isNull()) {
            continue;  // not an entity state object
        }
        EntityState next;
        JsonVariantConst s = entry["s"];
        if (s.is<const char *>()) {
            next.state = s.as<const char *>();
        }
        applyAttrs(entry["a"].as<JsonObjectConst>(), next.attrs, AttrMode::Replace);

        auto it = entities_.find(id);
        if (it == entities_.end()) {
            if (entities_.size() >= kMaxEntries) {
                continue;
            }
            entities_.emplace(std::string(id), std::move(next));
            ++version_;
        } else if (it->second != next) {
            it->second = std::move(next);
            ++version_;
        }
    }
}

void EntityStore::applyChanged(JsonObjectConst changed) {
    for (JsonPairConst kv : changed) {
        auto it = entities_.find(keyOf(kv));
        if (it == entities_.end()) {
            continue;
        }
        JsonObjectConst diff = kv.value().as<JsonObjectConst>();
        EntityState next = it->second;
        for (JsonVariantConst key : diff["-"]["a"].as<JsonArrayConst>()) {
            if (key.is<const char *>()) {
                resetAttr(key.as<const char *>(), next.attrs);
            }
        }
        JsonObjectConst plus = diff["+"].as<JsonObjectConst>();
        JsonVariantConst s = plus["s"];
        if (s.is<const char *>()) {
            next.state = s.as<const char *>();
        }
        applyAttrs(plus["a"].as<JsonObjectConst>(), next.attrs, AttrMode::Merge);
        if (next != it->second) {
            it->second = std::move(next);
            ++version_;
        }
    }
}

void EntityStore::applyRemoved(JsonArrayConst removed) {
    for (JsonVariantConst id : removed) {
        if (!id.is<const char *>()) {
            continue;
        }
        auto it = entities_.find(std::string_view(id.as<const char *>()));
        if (it != entities_.end()) {
            entities_.erase(it);
            ++version_;
        }
    }
}

const EntityState *EntityStore::find(std::string_view entityId) const {
    auto it = entities_.find(entityId);
    return it == entities_.end() ? nullptr : &it->second;
}

void EntityStore::clear() {
    if (!entities_.empty() || snapshotSeen_) {
        ++version_;
    }
    entities_.clear();
    snapshotSeen_ = false;
}

}  // namespace ha
}  // namespace core
