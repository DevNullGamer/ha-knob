#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Home Assistant WebSocket API messages (plan.md §3.7): builders for what the device sends
// and a classifier for what it receives. Pure. Every function takes an optional ArduinoJson
// allocator (nullptr = ArduinoJson's default; the device passes a PSRAM allocator).
//
// Builders return compact JSON with the keys in exactly this order:
//   buildAuth("T")                    {"type":"auth","access_token":"T"}
//   buildPing(1)                      {"id":1,"type":"ping"}
//   buildSubscribeEntities(2, {a,b})  {"id":2,"type":"subscribe_entities","entity_ids":["a","b"]}
//   buildUnsubscribeEvents(3, 2)      {"id":3,"type":"unsubscribe_events","subscription":2}
//   buildGetStates(4)                 {"id":4,"type":"get_states"}
//   buildCallService(5, "light", "turn_on", {"brightness_pct":40}, "light.kitchen")
//     {"id":5,"type":"call_service","domain":"light","service":"turn_on",
//      "service_data":{"brightness_pct":40},"target":{"entity_id":"light.kitchen"}}
//     "service_data" is left out when serviceData is null; "target" when entityId is "".
// The device never sends supported_features/coalesce_messages, so every frame holds
// exactly one message (a top-level array is classified as Error).
//
// Every message is parsed with an ArduinoJson filter and NestingLimit(kMaxNesting). The
// envelope filter keeps just "type", "id", "success", "ha_version", "message" and
// "error.code" / "error.message". Messages up to kEnvelopeOnlyBytes additionally keep the
// whole "result" and the subscribe_entities payload (plan-step6.md §3.3):
//   event.a.*.{s, a.{kAttributeKeys}}
//   event.c.*.{"+".{s, a.{kAttributeKeys}}, "-".a}
//   event.r
// so unneeded attributes (pictures, source lists, contexts) are skipped and never stored
// (they hit no string length limit). Nesting counts every object / array including the
// root, also inside skipped values: an "a" entity attribute may nest 15 levels, a "c" one
// 14; deeper messages are Error.
// Messages longer than kEnvelopeOnlyBytes are parsed envelope-only, so a multi-MB message
// (e.g. a get_states result) never builds a document of its own size. Such a Message has
// envelopeOnly set and an empty result() / event() (hasAdded/hasChanged/hasRemoved stay
// false).
namespace core {
namespace ha {

constexpr size_t kEnvelopeOnlyBytes = 256u * 1024u;
// ArduinoJson nesting limit for every HA JSON parse (the loop task stack is 8 KB and the
// parser recurses).
constexpr uint8_t kMaxNesting = 20;

std::string buildAuth(const std::string &accessToken, ArduinoJson::Allocator *allocator = nullptr);
std::string buildPing(uint32_t id, ArduinoJson::Allocator *allocator = nullptr);
std::string buildSubscribeEntities(uint32_t id, const std::vector<std::string> &entityIds,
                                   ArduinoJson::Allocator *allocator = nullptr);
std::string buildUnsubscribeEvents(uint32_t id, uint32_t subscription,
                                   ArduinoJson::Allocator *allocator = nullptr);
std::string buildGetStates(uint32_t id, ArduinoJson::Allocator *allocator = nullptr);
std::string buildCallService(uint32_t id, const std::string &domain, const std::string &service,
                             JsonVariantConst serviceData, const std::string &entityId,
                             ArduinoJson::Allocator *allocator = nullptr);

enum class MessageType : uint8_t {
    AuthRequired,  // {"type":"auth_required","ha_version":..}
    AuthOk,        // {"type":"auth_ok","ha_version":..}
    AuthInvalid,   // {"type":"auth_invalid","message":..}
    Result,        // {"id":n,"type":"result","success":bool,"result":..|"error":{code,message}}
    Pong,          // {"id":n,"type":"pong"}
    Event,         // {"id":n,"type":"event","event":{..}}
    Ignored,       // a JSON object with any other "type" string
    Error,         // not JSON, not an object, or "type" missing / not a string
};

// "auth_required", "auth_ok", "auth_invalid", "result", "pong", "event", "ignored", "error".
const char *messageTypeName(MessageType type);

struct Message {
    explicit Message(ArduinoJson::Allocator *allocator = nullptr);

    MessageType type = MessageType::Error;
    uint32_t id = 0;         // Result, Pong, Event; 0 if absent or not an unsigned integer
    bool success = false;    // Result: "success" is true
    std::string haVersion;   // AuthRequired, AuthOk ("" if absent)
    std::string message;     // AuthInvalid: "message"; failed Result: "error.message"
    std::string errorCode;   // failed Result: "error.code"
    // Event payload of subscribe_entities: which of "a" (added / initial state), "c"
    // (changed) and "r" (removed) the "event" object carries.
    bool hasAdded = false;
    bool hasChanged = false;
    bool hasRemoved = false;
    bool envelopeOnly = false;  // parsed with the envelope filter (length > kEnvelopeOnlyBytes)
    // Error only: ArduinoJson's error ("IncompleteInput", "InvalidInput", "NoMemory",
    // "TooDeep", "EmptyInput"), "not_object" or "no_type". "" otherwise. Static string.
    const char *parseError = "";

    JsonDocument doc;  // the parsed message (null for Error)

    JsonVariantConst result() const { return doc["result"]; }  // Result payload
    JsonVariantConst event() const { return doc["event"]; }    // Event payload
};

// Parses and classifies one received text frame. Never throws; malformed input gives
// type Error.
Message parseMessage(const char *json, size_t length, ArduinoJson::Allocator *allocator = nullptr);

}  // namespace ha
}  // namespace core
