#pragma once

#include <cstddef>
#include <cstdint>

// Incremental WebSocket frame receiver for the client side (RFC 6455 section 5). Pure: the
// device adapter reads sockets and calls feed(); message memory comes from a BufferProvider
// (PSRAM on the device, a vector stub in tests).
//
// feed() consumes bytes up to and including the end of at most one event, so the caller loops
// until all bytes are used. Results are identical whether the stream is fed byte by byte or in
// one buffer.
namespace core {
namespace ws {

class BufferProvider {
public:
    virtual ~BufferProvider() = default;

    // realloc semantics: returns a buffer of at least `total` bytes whose first bytes keep the
    // contents of `current` (nullptr = no buffer yet). On failure returns nullptr and `current`
    // stays valid (the receiver then releases it itself).
    virtual uint8_t *grow(uint8_t *current, size_t total) = 0;

    // Frees a buffer obtained from grow(). Never called with nullptr.
    virtual void release(uint8_t *buffer) = 0;
};

enum class EventType : uint8_t {
    None,           // need more bytes
    Text,           // complete text message (data NUL-terminated, length excludes the NUL)
    Ping,           // data/length = payload (<= 125)
    Pong,           // data/length = payload (<= 125)
    Close,          // closeCode (1005 if no body); data/length = whole body incl. the 2 code
                    // bytes, reason = NUL-terminated text after them
    Dropped,        // a message was discarded (length = payload bytes seen, reason below)
    ProtocolError,  // fatal; the receiver ignores all input until reset()
};

// Reasons (static strings):
//  Dropped:       "too_large", "no_memory", "binary"
//  ProtocolError: "masked_frame", "reserved_bits", "bad_opcode", "bad_control" (also a 1-byte Close body),
//                 "unexpected_continuation", "expected_continuation", "bad_length"
//  Close:         the server's UTF-8 text (not validated), "" if none
struct Event {
    EventType type = EventType::None;
    const uint8_t *data = nullptr;  // valid until the next feed() or reset()
    size_t length = 0;
    uint16_t closeCode = 0;
    const char *reason = "";        // same lifetime as data
};

struct Limits {
    size_t maxMessageBytes = 4u * 1024u * 1024u;  // larger messages are dropped ("too_large")
};

class WsReceiver {
public:
    // provider may be nullptr: every text message is then dropped with "no_memory".
    explicit WsReceiver(BufferProvider *provider, Limits limits = Limits());
    ~WsReceiver();
    WsReceiver(const WsReceiver &) = delete;
    WsReceiver &operator=(const WsReceiver &) = delete;

    // Returns the number of bytes consumed from data (<= len). If event.type is None all bytes
    // were consumed and more are needed; otherwise the event is complete and the remaining
    // bytes (if any) must be passed to the next feed(). Releases the previous event's buffer.
    size_t feed(const uint8_t *data, size_t len, Event &event);

    // Back to the initial state: frees any buffer, forgets a partial frame/message and clears a
    // protocol error (used after a reconnect).
    void reset();

    // Frees the buffer of the last Text event now instead of on the next feed()/reset(), so a
    // large message does not keep its memory while the connection is idle. The event's data
    // pointer is invalid afterwards. No-op if no Text buffer is held.
    void releaseDelivered();

    // Payload bytes of the data message in progress (0 if none), including bytes that are
    // being discarded.
    size_t messageBytesSoFar() const;

private:
    enum class State : uint8_t { Header, Store, Skip, Control };

    bool checkFirstBytes(Event &ev);
    bool beginFrame(Event &ev);
    bool completeFrame(Event &ev);
    bool fail(Event &ev, const char *reason);
    void startDiscard(const char *reason);
    void releaseBuffer();

    BufferProvider *provider_;
    Limits limits_;

    State state_ = State::Header;
    bool dead_ = false;

    // frame header
    uint8_t hdr_[10] = {0};
    size_t hdrLen_ = 0;
    size_t hdrNeeded_ = 2;

    // current frame
    bool fin_ = false;
    uint8_t op_ = 0;
    uint64_t remaining_ = 0;

    // current data message
    bool msgOpen_ = false;          // a fragmented message awaits its FIN frame
    uint8_t msgKind_ = 0;           // opcode of the first frame
    uint64_t msgTotal_ = 0;         // payload bytes seen (stored or discarded)
    size_t stored_ = 0;             // bytes written to buf_
    const char *discard_ = nullptr; // reason while the message is being discarded
    uint8_t *buf_ = nullptr;
    size_t bufCap_ = 0;             // capacity granted by the provider for buf_
    bool delivered_ = false;        // buf_ belongs to the last Text event

    // control frame payload (+1 for a terminating NUL after a Close reason)
    uint8_t ctrl_[126] = {0};
    size_t ctrlLen_ = 0;
};

}  // namespace ws
}  // namespace core
