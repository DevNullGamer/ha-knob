#include "ha/HaProtocol.h"

#include <cstring>

#include "ha/EntityAttrs.h"

namespace core {
namespace ha {

namespace {

JsonDocument makeDoc(ArduinoJson::Allocator *allocator) {
    return allocator != nullptr ? JsonDocument(allocator) : JsonDocument();
}

std::string serialize(const JsonDocument &doc) {
    std::string out;
    serializeJson(doc, out);
    return out;
}

// A document with "id" and "type" (in this order) for the command builders.
JsonDocument makeCommand(uint32_t id, const char *type, ArduinoJson::Allocator *allocator) {
    JsonDocument doc = makeDoc(allocator);
    doc["id"] = id;
    doc["type"] = type;
    return doc;
}

// The fields parseMessage() looks at; with payload also the result and the
// subscribe_entities event (see HaProtocol.h).
JsonDocument makeMessageFilter(bool withPayload, ArduinoJson::Allocator *allocator) {
    JsonDocument filter = makeDoc(allocator);
    filter["type"] = true;
    filter["id"] = true;
    filter["success"] = true;
    filter["ha_version"] = true;
    filter["message"] = true;
    filter["error"]["code"] = true;
    filter["error"]["message"] = true;
    if (withPayload) {
        filter["result"] = true;
        JsonObject event = filter["event"].to<JsonObject>();
        JsonObject added = event["a"]["*"].to<JsonObject>();
        added["s"] = true;
        addAttributeFilter(added["a"].to<JsonObject>());
        JsonObject changed = event["c"]["*"].to<JsonObject>();
        changed["+"]["s"] = true;
        addAttributeFilter(changed["+"]["a"].to<JsonObject>());
        changed["-"]["a"] = true;
        event["r"] = true;
    }
    return filter;
}

std::string stringOf(JsonVariantConst v) {
    const char *s = v.as<const char *>();
    return s != nullptr && v.is<const char *>() ? std::string(s) : std::string();
}

}  // namespace

std::string buildAuth(const std::string &accessToken, ArduinoJson::Allocator *allocator) {
    JsonDocument doc = makeDoc(allocator);
    doc["type"] = "auth";
    doc["access_token"] = accessToken;
    return serialize(doc);
}

std::string buildPing(uint32_t id, ArduinoJson::Allocator *allocator) {
    return serialize(makeCommand(id, "ping", allocator));
}

std::string buildSubscribeEntities(uint32_t id, const std::vector<std::string> &entityIds,
                                   ArduinoJson::Allocator *allocator) {
    JsonDocument doc = makeCommand(id, "subscribe_entities", allocator);
    JsonArray ids = doc["entity_ids"].to<JsonArray>();
    for (const std::string &entityId : entityIds) {
        ids.add(entityId);
    }
    return serialize(doc);
}

std::string buildUnsubscribeEvents(uint32_t id, uint32_t subscription, ArduinoJson::Allocator *allocator) {
    JsonDocument doc = makeCommand(id, "unsubscribe_events", allocator);
    doc["subscription"] = subscription;
    return serialize(doc);
}

std::string buildGetStates(uint32_t id, ArduinoJson::Allocator *allocator) {
    return serialize(makeCommand(id, "get_states", allocator));
}

std::string buildCallService(uint32_t id, const std::string &domain, const std::string &service,
                             JsonVariantConst serviceData, const std::string &entityId,
                             ArduinoJson::Allocator *allocator) {
    JsonDocument doc = makeCommand(id, "call_service", allocator);
    doc["domain"] = domain;
    doc["service"] = service;
    if (!serviceData.isNull()) {
        doc["service_data"] = serviceData;
    }
    if (!entityId.empty()) {
        doc["target"]["entity_id"] = entityId;
    }
    return serialize(doc);
}

const char *messageTypeName(MessageType type) {
    switch (type) {
        case MessageType::AuthRequired: return "auth_required";
        case MessageType::AuthOk: return "auth_ok";
        case MessageType::AuthInvalid: return "auth_invalid";
        case MessageType::Result: return "result";
        case MessageType::Pong: return "pong";
        case MessageType::Event: return "event";
        case MessageType::Ignored: return "ignored";
        case MessageType::Error:
        default: return "error";
    }
}

Message::Message(ArduinoJson::Allocator *allocator) : doc(makeDoc(allocator)) {}

Message parseMessage(const char *json, size_t length, ArduinoJson::Allocator *allocator, bool envelopeOnly) {
    Message msg(allocator);
    DeserializationError err = DeserializationError::InvalidInput;
    if (json != nullptr && length > 0) {
        msg.envelopeOnly = envelopeOnly || length > kEnvelopeOnlyBytes;
        const JsonDocument filter = makeMessageFilter(!msg.envelopeOnly, allocator);
        err = deserializeJson(msg.doc, json, length, DeserializationOption::Filter(filter),
                              DeserializationOption::NestingLimit(kMaxNesting));
    }
    if (err != DeserializationError::Ok || !msg.doc.is<JsonObjectConst>()) {
        msg.parseError = (err != DeserializationError::Ok) ? err.c_str() : "not_object";
        msg.doc.clear();
        msg.type = MessageType::Error;
        return msg;
    }
    JsonObjectConst root = msg.doc.as<JsonObjectConst>();
    JsonVariantConst typeValue = root["type"];
    if (!typeValue.is<const char *>()) {
        msg.parseError = "no_type";
        msg.type = MessageType::Error;
        return msg;
    }
    if (root["id"].is<uint32_t>()) {
        msg.id = root["id"].as<uint32_t>();
    }

    const char *type = typeValue.as<const char *>();
    if (std::strcmp(type, "auth_required") == 0) {
        msg.type = MessageType::AuthRequired;
        msg.haVersion = stringOf(root["ha_version"]);
    } else if (std::strcmp(type, "auth_ok") == 0) {
        msg.type = MessageType::AuthOk;
        msg.haVersion = stringOf(root["ha_version"]);
    } else if (std::strcmp(type, "auth_invalid") == 0) {
        msg.type = MessageType::AuthInvalid;
        msg.message = stringOf(root["message"]);
    } else if (std::strcmp(type, "result") == 0) {
        msg.type = MessageType::Result;
        msg.success = root["success"].is<bool>() && root["success"].as<bool>();
        if (!msg.success) {
            msg.errorCode = stringOf(root["error"]["code"]);
            msg.message = stringOf(root["error"]["message"]);
        }
    } else if (std::strcmp(type, "pong") == 0) {
        msg.type = MessageType::Pong;
    } else if (std::strcmp(type, "event") == 0) {
        msg.type = MessageType::Event;
        JsonObjectConst event = root["event"].as<JsonObjectConst>();
        if (!event.isNull()) {
            msg.hasAdded = !event["a"].isNull();
            msg.hasChanged = !event["c"].isNull();
            msg.hasRemoved = !event["r"].isNull();
        }
    } else {
        msg.type = MessageType::Ignored;
    }
    return msg;
}

}  // namespace ha
}  // namespace core
