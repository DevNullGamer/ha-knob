// Native tests for the WebSocket client codec (plan-step5b.md section 3.2, Step 5b.1):
// WsHandshake, WsFrame, WsReceiver. Every receiver case is fed both as one buffer and byte by
// byte and must produce identical events and identical buffer requests.
#include <unity.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include "util/Base64.h"
#include "ws/WsFrame.h"
#include "ws/WsHandshake.h"
#include "ws/WsReceiver.h"

using namespace core::ws;

typedef std::vector<uint8_t> Bytes;

void setUp() {}
void tearDown() {}

// ============================================================================================
// Handshake
// ============================================================================================

static const char *kRfcKey = "dGhlIHNhbXBsZSBub25jZQ==";
static const char *kRfcAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

static void test_accept_rfc_example() {
    TEST_ASSERT_EQUAL_STRING(kRfcAccept, expectedAccept(kRfcKey).c_str());
}

static uint32_t g_rngIdx = 0;
static uint32_t fixedRng() {
    static const uint32_t vals[4] = {0x00010203u, 0x04050607u, 0x08090A0Bu, 0x0C0D0E0Fu};
    return vals[(g_rngIdx++) & 3u];
}

static void test_key_from_fixed_rng() {
    g_rngIdx = 0;
    const std::string key = makeWebSocketKey(fixedRng);
    TEST_ASSERT_EQUAL_UINT32(4, g_rngIdx);  // exactly 4 draws
    TEST_ASSERT_EQUAL_UINT32(24, (uint32_t)key.size());
    std::vector<uint8_t> raw;
    TEST_ASSERT_TRUE(core::util::base64Decode(key, raw));
    TEST_ASSERT_EQUAL_UINT32(16, (uint32_t)raw.size());
    for (size_t i = 0; i < 16; ++i) TEST_ASSERT_EQUAL_UINT8((uint8_t)i, raw[i]);  // big-endian
    TEST_ASSERT_EQUAL_STRING("AAECAwQFBgcICQoLDA0ODw==", key.c_str());
}

static void test_upgrade_request_ws_80() {
    const std::string r = buildUpgradeRequest("192.168.1.10", 80, false, "/api/websocket", "KEY=");
    TEST_ASSERT_EQUAL_STRING(
        "GET /api/websocket HTTP/1.1\r\n"
        "Host: 192.168.1.10\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: KEY=\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        r.c_str());
}

static void test_upgrade_request_ws_8123() {
    const std::string r = buildUpgradeRequest("ha.local", 8123, false, "/api/websocket", "KEY=");
    TEST_ASSERT_EQUAL_STRING(
        "GET /api/websocket HTTP/1.1\r\n"
        "Host: ha.local:8123\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: KEY=\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        r.c_str());
}

static void test_upgrade_request_wss_443() {
    const std::string r = buildUpgradeRequest("ha.example.com", 443, true, "/api/websocket", "KEY=");
    TEST_ASSERT_EQUAL_STRING(
        "GET /api/websocket HTTP/1.1\r\n"
        "Host: ha.example.com\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: KEY=\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        r.c_str());
}

static void test_upgrade_request_port_defaults_are_per_scheme() {
    // 443 on plain ws and 80 on wss are not defaults -> port shown.
    TEST_ASSERT_NOT_NULL(strstr(buildUpgradeRequest("h", 443, false, "/p", "K").c_str(), "Host: h:443\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(buildUpgradeRequest("h", 80, true, "/p", "K").c_str(), "Host: h:80\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(buildUpgradeRequest("h", 8443, true, "/p", "K").c_str(), "Host: h:8443\r\n"));
}

static void test_upgrade_request_prefix_path_and_no_origin() {
    const std::string r = buildUpgradeRequest("ha.local", 8123, false, "/ha/prefix/api/websocket", "KEY=");
    TEST_ASSERT_EQUAL(0, r.compare(0, 41, "GET /ha/prefix/api/websocket HTTP/1.1\r\nHo"));
    TEST_ASSERT_EQUAL_STRING(
        "GET /ha/prefix/api/websocket HTTP/1.1\r\n"
        "Host: ha.local:8123\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: KEY=\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        r.c_str());
    TEST_ASSERT_TRUE(r.find("Origin") == std::string::npos);
    TEST_ASSERT_TRUE(r.find("Sec-WebSocket-Protocol") == std::string::npos);
}

static std::string buildResponse(const std::string &statusLine, const std::vector<std::string> &headers) {
    std::string s = statusLine + "\r\n";
    for (const std::string &h : headers) s += h + "\r\n";
    s += "\r\n";
    return s;
}

static std::string okResponse() {
    return buildResponse("HTTP/1.1 101 Switching Protocols",
                         {"Upgrade: websocket", "Connection: Upgrade",
                          std::string("Sec-WebSocket-Accept: ") + kRfcAccept});
}

static UpgradeResult parse(const std::string &s) { return parseUpgradeResponse(s.data(), s.size(), kRfcKey); }

static void test_response_ok() {
    const std::string s = okResponse();
    UpgradeResult r = parse(s);
    TEST_ASSERT_TRUE(r.status == UpgradeResult::Status::Ok);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)s.size(), (uint32_t)r.headerBytes);
    TEST_ASSERT_EQUAL(101, r.httpStatus);
    TEST_ASSERT_TRUE(r.error.empty());
}

static void test_response_mixed_case() {
    const std::string s = buildResponse(
        "HTTP/1.1 101 Switching Protocols",
        {"UPGRADE: WebSocket", "cOnNeCtIoN: uPgRaDe", std::string("sec-websocket-accept: ") + kRfcAccept,
         "Server: x"});
    TEST_ASSERT_TRUE(parse(s).status == UpgradeResult::Status::Ok);
}

static void test_response_connection_list() {
    const std::string s = buildResponse(
        "HTTP/1.1 101 Switching Protocols",
        {"Upgrade: websocket", "Connection: keep-alive, Upgrade", std::string("Sec-WebSocket-Accept: ") + kRfcAccept});
    TEST_ASSERT_TRUE(parse(s).status == UpgradeResult::Status::Ok);
    const std::string s2 = buildResponse(
        "HTTP/1.1 101 Switching Protocols",
        {"Upgrade: websocket", "Connection:   upgrade , keep-alive", std::string("Sec-WebSocket-Accept: ") + kRfcAccept});
    TEST_ASSERT_TRUE(parse(s2).status == UpgradeResult::Status::Ok);
}

static void test_response_connection_upgradefoo_rejected() {
    const std::string s = buildResponse(
        "HTTP/1.1 101 Switching Protocols",
        {"Upgrade: websocket", "Connection: upgradefoo", std::string("Sec-WebSocket-Accept: ") + kRfcAccept});
    UpgradeResult r = parse(s);
    TEST_ASSERT_TRUE(r.status == UpgradeResult::Status::Failed);
    TEST_ASSERT_EQUAL_STRING("bad_response", r.error.c_str());
}

static void test_response_incomplete_and_split_terminator() {
    const std::string s = okResponse();
    for (size_t cut = 0; cut < s.size(); ++cut) {  // every prefix, incl. splits inside \r\n\r\n
        UpgradeResult r = parseUpgradeResponse(s.data(), cut, kRfcKey);
        char msg[32];
        std::snprintf(msg, sizeof(msg), "cut %u", unsigned(cut));
        TEST_ASSERT_TRUE_MESSAGE(r.status == UpgradeResult::Status::Incomplete, msg);
    }
    TEST_ASSERT_TRUE(parseUpgradeResponse(s.data(), s.size(), kRfcKey).status == UpgradeResult::Status::Ok);
}

static void test_response_bad_accept() {
    const std::string s = buildResponse("HTTP/1.1 101 Switching Protocols",
                                        {"Upgrade: websocket", "Connection: Upgrade",
                                         "Sec-WebSocket-Accept: AAAAAAAAAAAAAAAAAAAAAAAAAAA="});
    UpgradeResult r = parse(s);
    TEST_ASSERT_TRUE(r.status == UpgradeResult::Status::Failed);
    TEST_ASSERT_EQUAL_STRING("bad_accept", r.error.c_str());
    TEST_ASSERT_EQUAL_UINT32((uint32_t)s.size(), (uint32_t)r.headerBytes);

    // accept value is compared exactly (case-sensitive)
    std::string lower = kRfcAccept;
    for (char &c : lower) c = (char)tolower((unsigned char)c);
    const std::string s2 = buildResponse(
        "HTTP/1.1 101 Switching Protocols",
        {"Upgrade: websocket", "Connection: Upgrade", "Sec-WebSocket-Accept: " + lower});
    TEST_ASSERT_EQUAL_STRING("bad_accept", parse(s2).error.c_str());
}

static void test_response_missing_accept_is_bad_accept() {
    const std::string s = buildResponse("HTTP/1.1 101 Switching Protocols", {"Upgrade: websocket", "Connection: Upgrade"});
    UpgradeResult r = parse(s);
    TEST_ASSERT_TRUE(r.status == UpgradeResult::Status::Failed);
    TEST_ASSERT_EQUAL_STRING("bad_accept", r.error.c_str());
}

static void test_response_missing_upgrade_or_connection_is_bad_response() {
    const std::string acc = std::string("Sec-WebSocket-Accept: ") + kRfcAccept;
    UpgradeResult r = parse(buildResponse("HTTP/1.1 101 Switching Protocols", {"Connection: Upgrade", acc}));
    TEST_ASSERT_TRUE(r.status == UpgradeResult::Status::Failed);
    TEST_ASSERT_EQUAL_STRING("bad_response", r.error.c_str());

    r = parse(buildResponse("HTTP/1.1 101 Switching Protocols", {"Upgrade: websocket", acc}));
    TEST_ASSERT_EQUAL_STRING("bad_response", r.error.c_str());

    r = parse(buildResponse("HTTP/1.1 101 Switching Protocols", {"Upgrade: h2c", "Connection: Upgrade", acc}));
    TEST_ASSERT_EQUAL_STRING("bad_response", r.error.c_str());

    r = parse(buildResponse("HTTP/1.1 101 Switching Protocols", {"Upgrade: websocket2", "Connection: Upgrade", acc}));
    TEST_ASSERT_EQUAL_STRING("bad_response", r.error.c_str());
}

static void test_response_malformed_header_line() {
    UpgradeResult r = parse(buildResponse("HTTP/1.1 101 Switching Protocols",
                                          {"Upgrade: websocket", "no colon here", "Connection: Upgrade"}));
    TEST_ASSERT_TRUE(r.status == UpgradeResult::Status::Failed);
    TEST_ASSERT_EQUAL_STRING("bad_response", r.error.c_str());
}

static void test_response_http_errors() {
    std::string s = buildResponse("HTTP/1.1 502 Bad Gateway", {"Content-Length: 5"}) + "<html";
    UpgradeResult r = parse(s);
    TEST_ASSERT_TRUE(r.status == UpgradeResult::Status::Failed);
    TEST_ASSERT_EQUAL_STRING("http 502", r.error.c_str());
    TEST_ASSERT_EQUAL(502, r.httpStatus);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(s.size() - 5), (uint32_t)r.headerBytes);

    r = parse(buildResponse("HTTP/1.1 301 Moved Permanently", {"Location: https://x/"}));
    TEST_ASSERT_TRUE(r.status == UpgradeResult::Status::Failed);
    TEST_ASSERT_EQUAL_STRING("http 301", r.error.c_str());
    TEST_ASSERT_EQUAL(301, r.httpStatus);

    // 200 with otherwise perfect headers is still not an upgrade
    r = parse(buildResponse("HTTP/1.1 200 OK", {"Upgrade: websocket", "Connection: Upgrade",
                                                 std::string("Sec-WebSocket-Accept: ") + kRfcAccept}));
    TEST_ASSERT_EQUAL_STRING("http 200", r.error.c_str());
}

static void test_response_garbage_status_line() {
    UpgradeResult r = parse("garbage\r\n\r\n");
    TEST_ASSERT_TRUE(r.status == UpgradeResult::Status::Failed);
    TEST_ASSERT_EQUAL_STRING("bad_response", r.error.c_str());
    TEST_ASSERT_EQUAL(0, r.httpStatus);
    r = parse("HTTP/2 101 x\r\n\r\n");
    TEST_ASSERT_EQUAL_STRING("bad_response", r.error.c_str());
}

static std::string padResponseTo(size_t total) {
    std::string base = buildResponse("HTTP/1.1 101 Switching Protocols",
                                     {"Upgrade: websocket", "Connection: Upgrade",
                                      std::string("Sec-WebSocket-Accept: ") + kRfcAccept, "X-Pad: "});
    // base ends with "X-Pad: \r\n\r\n"; insert pad characters after "X-Pad: "
    const size_t pad = total - base.size();
    base.insert(base.size() - 4, std::string(pad, 'a'));
    return base;
}

static void test_response_4096_boundary() {
    const std::string at = padResponseTo(4096);
    TEST_ASSERT_EQUAL_UINT32(4096, (uint32_t)at.size());
    UpgradeResult r = parse(at);
    TEST_ASSERT_TRUE(r.status == UpgradeResult::Status::Ok);
    TEST_ASSERT_EQUAL_UINT32(4096, (uint32_t)r.headerBytes);

    const std::string over = padResponseTo(4097);
    r = parse(over);
    TEST_ASSERT_TRUE(r.status == UpgradeResult::Status::Failed);
    TEST_ASSERT_EQUAL_STRING("bad_response", r.error.c_str());

    // no terminator: 4095 bytes still incomplete, 4096 bytes failed
    const std::string junk = "HTTP/1.1 101 " + std::string(5000, 'a');
    TEST_ASSERT_TRUE(parseUpgradeResponse(junk.data(), 4095, kRfcKey).status == UpgradeResult::Status::Incomplete);
    r = parseUpgradeResponse(junk.data(), 4096, kRfcKey);
    TEST_ASSERT_TRUE(r.status == UpgradeResult::Status::Failed);
    TEST_ASSERT_EQUAL_STRING("bad_response", r.error.c_str());
}

// ============================================================================================
// Frame encoder
// ============================================================================================

static Bytes payloadOf(size_t n) {
    Bytes p(n);
    for (size_t i = 0; i < n; ++i) p[i] = (uint8_t)(i * 7 + 3);
    return p;
}

// Decodes the header of a client frame and verifies masking; returns payload offset.
static void checkClientFrame(const Bytes &f, uint8_t op, const Bytes &payload, uint32_t key, size_t expectHdr) {
    TEST_ASSERT_EQUAL_UINT8(0x80 | op, f[0]);
    TEST_ASSERT_TRUE((f[1] & 0x80) != 0);
    const size_t n = payload.size();
    size_t hdr;
    if (n <= 125) {
        TEST_ASSERT_EQUAL_UINT8(0x80 | n, f[1]);
        hdr = 2;
    } else if (n <= 65535) {
        TEST_ASSERT_EQUAL_UINT8(0x80 | 126, f[1]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(n >> 8), f[2]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)n, f[3]);
        hdr = 4;
    } else {
        TEST_ASSERT_EQUAL_UINT8(0x80 | 127, f[1]);
        for (int i = 0; i < 8; ++i) TEST_ASSERT_EQUAL_UINT8((uint8_t)(((uint64_t)n) >> (56 - 8 * i)), f[2 + i]);
        hdr = 10;
    }
    TEST_ASSERT_EQUAL_UINT32((uint32_t)expectHdr, (uint32_t)hdr);
    const uint8_t k[4] = {(uint8_t)(key >> 24), (uint8_t)(key >> 16), (uint8_t)(key >> 8), (uint8_t)key};
    for (int i = 0; i < 4; ++i) TEST_ASSERT_EQUAL_UINT8(k[i], f[hdr + i]);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(hdr + 4 + n), (uint32_t)f.size());
    for (size_t i = 0; i < n; ++i) {
        if ((uint8_t)(payload[i] ^ k[i & 3]) != f[hdr + 4 + i]) TEST_FAIL_MESSAGE("masked payload mismatch");
    }
}

static void test_encode_lengths() {
    const size_t lens[] = {0, 1, 125, 126, 127, 65535, 65536, 100000};
    const size_t hdrs[] = {2, 2, 2, 4, 4, 4, 10, 10};
    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); ++i) {
        const Bytes p = payloadOf(lens[i]);
        const uint32_t key = 0xA1B2C3D4u;
        Bytes f = encodeClientFrame(Opcode::Text, p.empty() ? nullptr : p.data(), p.size(), key);
        checkClientFrame(f, 0x1, p, key, hdrs[i]);
    }
}

static void test_encode_64bit_length_bytes() {
    const Bytes p = payloadOf(65536);
    Bytes f = encodeClientFrame(Opcode::Binary, p.data(), p.size(), 0);
    const uint8_t expect[10] = {0x82, 0xFF, 0, 0, 0, 0, 0, 1, 0, 0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expect, f.data(), 10);
    // key 0 -> payload unchanged
    TEST_ASSERT_EQUAL_UINT8_ARRAY(p.data(), f.data() + 14, p.size());
}

static void test_encode_rfc_hello() {
    const uint8_t hello[5] = {'H', 'e', 'l', 'l', 'o'};
    Bytes f = encodeClientFrame(Opcode::Text, hello, 5, 0x37fa213du);
    const uint8_t expect[11] = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58};
    TEST_ASSERT_EQUAL_UINT32(11, (uint32_t)f.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expect, f.data(), 11);
}

static void test_encode_other_opcodes() {
    const uint8_t pl[2] = {1, 2};
    Bytes f = encodeClientFrame(Opcode::Ping, pl, 2, 0);
    TEST_ASSERT_EQUAL_UINT8(0x89, f[0]);
    TEST_ASSERT_EQUAL_UINT8(0x82, f[1]);
    f = encodeClientFrame(Opcode::Pong, pl, 2, 0);
    TEST_ASSERT_EQUAL_UINT8(0x8A, f[0]);
    f = encodeClientFrame(Opcode::Binary, nullptr, 0, 0x01020304u);
    const uint8_t expect[6] = {0x82, 0x80, 1, 2, 3, 4};
    TEST_ASSERT_EQUAL_UINT32(6, (uint32_t)f.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expect, f.data(), 6);
}

static void test_encode_close_1000() {
    Bytes f = encodeClose(1000, 0x37fa213du);
    const uint8_t expect[8] = {0x88, 0x82, 0x37, 0xfa, 0x21, 0x3d, 0x03 ^ 0x37, 0xE8 ^ 0xfa};
    TEST_ASSERT_EQUAL_UINT32(8, (uint32_t)f.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expect, f.data(), 8);
    f = encodeClose(1002, 0);
    const uint8_t e2[8] = {0x88, 0x82, 0, 0, 0, 0, 0x03, 0xEA};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(e2, f.data(), 8);
}

// ============================================================================================
// Receiver test infrastructure
// ============================================================================================

struct ProviderCfg {
    int failCall = 0;              // 1-based grow() call that returns nullptr (0 = none)
    size_t failAbove = SIZE_MAX;   // grow() with total above this returns nullptr
    bool moveAlways = true;        // always return a different pointer (copy), like a worst realloc
};

class CountingProvider : public BufferProvider {
public:
    explicit CountingProvider(ProviderCfg c = ProviderCfg()) : cfg(c) {}
    ~CountingProvider() override {
        for (auto &kv : live) free(kv.first);
    }

    uint8_t *grow(uint8_t *current, size_t total) override {
        ++growCalls;
        requests.push_back(total);
        if (current != nullptr && live.count(current) == 0) misuse = true;
        if (total == 0) misuse = true;
        if (growCalls == cfg.failCall || total > cfg.failAbove) return nullptr;  // current stays valid
        uint8_t *nb;
        if (current == nullptr || cfg.moveAlways) {
            nb = (uint8_t *)malloc(total);
            memset(nb, 0xEE, total);  // garbage: a missing NUL terminator would show
            if (current != nullptr) {
                size_t old = live[current];
                memcpy(nb, current, old < total ? old : total);
                free(current);
                live.erase(current);
            }
        } else {
            size_t old = live[current];
            nb = (uint8_t *)realloc(current, total);
            live.erase(current);
            if (total > old) memset(nb + old, 0xEE, total - old);
        }
        live[nb] = total;
        return nb;
    }

    void release(uint8_t *buffer) override {
        ++releases;
        if (buffer == nullptr || live.count(buffer) == 0) {
            misuse = true;
            return;
        }
        free(buffer);
        live.erase(buffer);
    }

    ProviderCfg cfg;
    int growCalls = 0;
    int releases = 0;
    bool misuse = false;
    std::vector<size_t> requests;
    std::map<uint8_t *, size_t> live;
};

static void assertBalanced(const CountingProvider &p) {
    TEST_ASSERT_FALSE_MESSAGE(p.misuse, "provider misuse (release of unknown/null buffer, grow of unknown current)");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, (uint32_t)p.live.size(), "buffers still live: grow/release unbalanced");
}

struct Rec {
    EventType type = EventType::None;
    std::string data;
    size_t length = 0;
    uint16_t code = 0;
    std::string reason;
    bool nul = true;
    bool operator==(const Rec &o) const {
        return type == o.type && data == o.data && length == o.length && code == o.code && reason == o.reason &&
               nul == o.nul;
    }
};

static Rec collect(const Event &e) {
    Rec r;
    r.type = e.type;
    r.length = e.length;
    r.code = e.closeCode;
    r.reason = e.reason ? e.reason : "";
    if (e.data != nullptr) {
        r.data.assign((const char *)e.data, e.length);
        if (e.type == EventType::Text) r.nul = (e.data[e.length] == 0);
    }
    return r;
}

// Feeds `in` to a fresh receiver; returns all events. Checks the feed() contract and that the
// provider is balanced once the receiver is destroyed.
static std::vector<Rec> runOnce(const Bytes &in, bool bytewise, CountingProvider *prov, Limits lim) {
    std::vector<Rec> out;
    {
        WsReceiver rx(prov, lim);
        size_t off = 0;
        int guard = 0;
        while (off < in.size()) {
            TEST_ASSERT_TRUE_MESSAGE(++guard < 100000000, "runaway loop");
            const size_t chunk = bytewise ? 1 : in.size() - off;
            Event e;
            const size_t used = rx.feed(in.data() + off, chunk, e);
            TEST_ASSERT_TRUE_MESSAGE(used <= chunk, "feed consumed more than offered");
            TEST_ASSERT_TRUE_MESSAGE(used > 0 || e.type != EventType::None, "feed made no progress");
            if (e.type == EventType::None) {
                TEST_ASSERT_TRUE_MESSAGE(used == chunk, "None event but input left over");
            } else {
                out.push_back(collect(e));
            }
            off += used;
        }
    }
    if (prov != nullptr) assertBalanced(*prov);
    return out;
}

struct Outcome {
    std::vector<Rec> ev;
    std::vector<size_t> requests;
    int growCalls = 0;
};

// Runs whole-buffer and byte-by-byte with identical provider config and asserts equality.
static Outcome both(const Bytes &in, Limits lim = Limits(), ProviderCfg pc = ProviderCfg()) {
    CountingProvider p1(pc), p2(pc);
    std::vector<Rec> whole = runOnce(in, false, &p1, lim);
    std::vector<Rec> bytewise = runOnce(in, true, &p2, lim);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)whole.size(), (uint32_t)bytewise.size(), "event count differs");
    for (size_t i = 0; i < whole.size(); ++i) {
        char msg[48];
        std::snprintf(msg, sizeof(msg), "event %u differs whole vs bytewise", unsigned(i));
        TEST_ASSERT_TRUE_MESSAGE(whole[i] == bytewise[i], msg);
    }
    TEST_ASSERT_TRUE_MESSAGE(p1.requests == p2.requests, "grow() requests differ whole vs bytewise");
    Outcome o;
    o.ev = whole;
    o.requests = p1.requests;
    o.growCalls = p1.growCalls;
    return o;
}

static Limits maxOf(size_t n) {
    Limits l;
    l.maxMessageBytes = n;
    return l;
}

// ---- server frame builders (unmasked) ----

static Bytes frame(bool fin, uint8_t op, const Bytes &p, uint8_t rsv = 0) {
    Bytes f;
    f.push_back((uint8_t)((fin ? 0x80 : 0) | rsv | op));
    const size_t n = p.size();
    if (n <= 125) {
        f.push_back((uint8_t)n);
    } else if (n <= 65535) {
        f.push_back(126);
        f.push_back((uint8_t)(n >> 8));
        f.push_back((uint8_t)n);
    } else {
        f.push_back(127);
        for (int i = 7; i >= 0; --i) f.push_back((uint8_t)(((uint64_t)n) >> (8 * i)));
    }
    f.insert(f.end(), p.begin(), p.end());
    return f;
}

static Bytes S(const std::string &s) { return Bytes(s.begin(), s.end()); }

static Bytes text(const std::string &s) { return frame(true, 0x1, S(s)); }

static Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes r;
    for (const Bytes &b : parts) r.insert(r.end(), b.begin(), b.end());
    return r;
}

static std::string pattern(size_t n) {
    std::string s(n, 'a');
    for (size_t i = 0; i < n; ++i) s[i] = (char)('a' + (i % 26));
    return s;
}

static void expectText(const Rec &r, const std::string &s) {
    TEST_ASSERT_TRUE(r.type == EventType::Text);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)s.size(), (uint32_t)r.length);
    TEST_ASSERT_TRUE_MESSAGE(r.data == s, "text content differs");
    TEST_ASSERT_TRUE_MESSAGE(r.nul, "text not NUL terminated");
}

static void expectDropped(const Rec &r, size_t len, const char *reason) {
    TEST_ASSERT_TRUE(r.type == EventType::Dropped);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)len, (uint32_t)r.length);
    TEST_ASSERT_EQUAL_STRING(reason, r.reason.c_str());
}

static void expectProtoError(const Rec &r, const char *reason) {
    TEST_ASSERT_TRUE(r.type == EventType::ProtocolError);
    TEST_ASSERT_EQUAL_STRING(reason, r.reason.c_str());
}

// ============================================================================================
// Receiver: happy paths
// ============================================================================================

static void test_rx_text_sizes() {
    const size_t sizes[] = {0, 1, 125, 126, 127, 65535, 65536, 70000};
    for (size_t n : sizes) {
        const std::string s = pattern(n);
        Outcome o = both(text(s));
        TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)o.ev.size());
        expectText(o.ev[0], s);
    }
}

static void test_rx_text_with_embedded_nul() {
    std::string s("ab\0cd", 5);
    Outcome o = both(text(s));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)o.ev.size());
    expectText(o.ev[0], s);
}

static void test_rx_two_messages() {
    Outcome o = both(cat({text("first"), text("second message")}));
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)o.ev.size());
    expectText(o.ev[0], "first");
    expectText(o.ev[1], "second message");
}

static void test_rx_many_messages_and_empty() {
    Bytes in;
    for (int i = 0; i < 20; ++i) in = cat({in, text(i % 3 == 0 ? "" : pattern(i))});
    Outcome o = both(in);
    TEST_ASSERT_EQUAL_UINT32(20, (uint32_t)o.ev.size());
    for (int i = 0; i < 20; ++i) expectText(o.ev[i], i % 3 == 0 ? "" : pattern(i));
}

static void test_rx_fragmented_with_ping_between() {
    Bytes in = cat({frame(false, 0x1, S("Hel")), frame(true, 0x9, S("p")), frame(false, 0x0, S("lo ")),
                    frame(true, 0x0, S("world"))});
    Outcome o = both(in);
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)o.ev.size());
    TEST_ASSERT_TRUE(o.ev[0].type == EventType::Ping);
    TEST_ASSERT_EQUAL_STRING("p", o.ev[0].data.c_str());
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)o.ev[0].length);
    expectText(o.ev[1], "Hello world");
}

static void test_rx_fragmented_large_content() {
    const std::string s = pattern(5000);
    Bytes in;
    for (size_t off = 0; off < s.size(); off += 700) {
        const size_t n = std::min<size_t>(700, s.size() - off);
        in = cat({in, frame(off + n >= s.size(), off == 0 ? 0x1 : 0x0, S(s.substr(off, n)))});
    }
    Outcome o = both(in);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)o.ev.size());
    expectText(o.ev[0], s);
}

static void test_rx_ping_pong_payloads() {
    const std::string p125 = pattern(125);
    Outcome o = both(cat({frame(true, 0x9, S("")), frame(true, 0x9, S(p125)), frame(true, 0xA, S("pong!")),
                          frame(true, 0xA, S(""))}));
    TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)o.ev.size());
    TEST_ASSERT_TRUE(o.ev[0].type == EventType::Ping);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)o.ev[0].length);
    TEST_ASSERT_TRUE(o.ev[1].type == EventType::Ping);
    TEST_ASSERT_TRUE(o.ev[1].data == p125);
    TEST_ASSERT_TRUE(o.ev[2].type == EventType::Pong);
    TEST_ASSERT_EQUAL_STRING("pong!", o.ev[2].data.c_str());
    TEST_ASSERT_TRUE(o.ev[3].type == EventType::Pong);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)o.ev[3].length);
}

static void test_rx_close_with_code_and_reason() {
    Bytes body = {0x03, 0xE9};  // 1001
    body.push_back('b');
    body.push_back('y');
    body.push_back('e');
    Outcome o = both(frame(true, 0x8, body));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)o.ev.size());
    const Rec &r = o.ev[0];
    TEST_ASSERT_TRUE(r.type == EventType::Close);
    TEST_ASSERT_EQUAL_UINT16(1001, r.code);
    TEST_ASSERT_EQUAL_UINT32(5, (uint32_t)r.length);  // whole body incl. 2 code bytes
    TEST_ASSERT_TRUE(r.data == std::string((const char *)body.data(), 5));
    TEST_ASSERT_EQUAL_STRING("bye", r.reason.c_str());
}

static void test_rx_close_code_only() {
    Outcome o = both(frame(true, 0x8, Bytes{0x03, 0xE8}));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)o.ev.size());
    TEST_ASSERT_TRUE(o.ev[0].type == EventType::Close);
    TEST_ASSERT_EQUAL_UINT16(1000, o.ev[0].code);
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)o.ev[0].length);
    TEST_ASSERT_EQUAL_STRING("", o.ev[0].reason.c_str());
}

static void test_rx_close_without_body_is_1005() {
    Outcome o = both(frame(true, 0x8, Bytes{}));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)o.ev.size());
    TEST_ASSERT_TRUE(o.ev[0].type == EventType::Close);
    TEST_ASSERT_EQUAL_UINT16(1005, o.ev[0].code);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)o.ev[0].length);
    TEST_ASSERT_EQUAL_STRING("", o.ev[0].reason.c_str());
}

static void test_rx_close_max_reason() {
    // 125-byte body: 2 code bytes + 123 reason bytes (reason must be NUL terminated, no overflow)
    Bytes body = {0x03, 0xE8};
    const std::string reason = pattern(123);
    body.insert(body.end(), reason.begin(), reason.end());
    Outcome o = both(frame(true, 0x8, body));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)o.ev.size());
    TEST_ASSERT_EQUAL_UINT32(125, (uint32_t)o.ev[0].length);
    TEST_ASSERT_TRUE(o.ev[0].reason == reason);
}

static void test_rx_close_one_byte_body_is_protocol_error() {
    Outcome o = both(frame(true, 0x8, Bytes{0x03}));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)o.ev.size());
    expectProtoError(o.ev[0], "bad_control");
}

static void test_rx_events_before_and_after_close() {
    Outcome o = both(cat({text("a"), frame(true, 0x8, Bytes{0x03, 0xE8}), text("b")}));
    // the receiver does not stop at Close; the caller decides
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)o.ev.size());
    expectText(o.ev[0], "a");
    TEST_ASSERT_TRUE(o.ev[1].type == EventType::Close);
    expectText(o.ev[2], "b");
}

static void test_rx_64bit_length_form_for_small_payload() {
    // A valid but non-minimal 64-bit length encoding of a 3-byte payload.
    Bytes f = {0x81, 127, 0, 0, 0, 0, 0, 0, 0, 3, 'a', 'b', 'c'};
    Outcome o = both(f);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)o.ev.size());
    expectText(o.ev[0], "abc");
}

// ============================================================================================
// Receiver: BufferProvider contract
// ============================================================================================

static void test_provider_single_frame_requests_len_plus_one() {
    const size_t sizes[] = {0, 1, 100, 125, 126, 65536};
    for (size_t n : sizes) {
        Outcome o = both(text(pattern(n)));
        TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)o.growCalls);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)(n + 1), (uint32_t)o.requests[0]);
    }
}

static void test_provider_first_grow_has_null_current_and_release_after_next_feed() {
    CountingProvider p;
    WsReceiver rx(&p);
    Bytes in = text("hello");
    Event e;
    size_t used = rx.feed(in.data(), in.size(), e);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)in.size(), (uint32_t)used);
    TEST_ASSERT_TRUE(e.type == EventType::Text);
    TEST_ASSERT_EQUAL_STRING("hello", (const char *)e.data);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)p.live.size());  // valid until next feed
    TEST_ASSERT_EQUAL_INT(0, p.releases);
    Event e2;
    const uint8_t one = 0x81;
    rx.feed(&one, 1, e2);  // start of the next frame releases the previous event's buffer
    TEST_ASSERT_TRUE(e2.type == EventType::None);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)p.live.size());
    TEST_ASSERT_EQUAL_INT(1, p.releases);
    assertBalanced(p);
}

static void test_provider_balanced_after_many_messages() {
    Bytes in;
    for (int i = 0; i < 50; ++i) in = cat({in, text(pattern((size_t)i * 3))});
    CountingProvider p;
    std::vector<Rec> ev = runOnce(in, false, &p, Limits());
    TEST_ASSERT_EQUAL_UINT32(50, (uint32_t)ev.size());
    TEST_ASSERT_EQUAL_INT(50, p.growCalls);
    TEST_ASSERT_EQUAL_INT(50, p.releases);
}

static void test_provider_geometric_growth_for_tiny_fragments() {
    Bytes in = frame(false, 0x1, S("x"));
    for (int i = 0; i < 998; ++i) in = cat({in, frame(false, 0x0, S("y"))});
    in = cat({in, frame(true, 0x0, S("z"))});
    Outcome o = both(in);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)o.ev.size());
    TEST_ASSERT_EQUAL_UINT32(1000, (uint32_t)o.ev[0].length);
    TEST_ASSERT_EQUAL_CHAR('x', o.ev[0].data[0]);
    TEST_ASSERT_EQUAL_CHAR('y', o.ev[0].data[500]);
    TEST_ASSERT_EQUAL_CHAR('z', o.ev[0].data[999]);
    TEST_ASSERT_TRUE_MESSAGE(o.growCalls <= 16, "too many grow() calls for 1000 tiny fragments");
    TEST_ASSERT_TRUE(o.growCalls >= 2);
    for (size_t i = 1; i < o.requests.size(); ++i) TEST_ASSERT_TRUE(o.requests[i] > o.requests[i - 1]);
}

static void test_provider_zero_length_continuations_do_not_grow() {
    Bytes in = frame(false, 0x1, S("ab"));
    for (int i = 0; i < 50; ++i) in = cat({in, frame(false, 0x0, S(""))});
    in = cat({in, frame(true, 0x0, S("c"))});
    Outcome o = both(in);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)o.ev.size());
    expectText(o.ev[0], "abc");
    TEST_ASSERT_EQUAL_INT(2, o.growCalls);  // first fragment + the "c" that no longer fits

    // empty final continuation also needs no grow
    Outcome o2 = both(cat({frame(false, 0x1, S("ab")), frame(true, 0x0, S(""))}));
    expectText(o2.ev[0], "ab");
    TEST_ASSERT_EQUAL_INT(1, o2.growCalls);
}

static void test_provider_null_provider_drops_with_no_memory() {
    Bytes in = cat({text("abc"), frame(false, 0x1, S("ab")), frame(true, 0x0, S("cd")), text("")});
    std::vector<Rec> whole = runOnce(in, false, nullptr, Limits());
    std::vector<Rec> bytewise = runOnce(in, true, nullptr, Limits());
    TEST_ASSERT_TRUE(whole == bytewise);
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)whole.size());
    expectDropped(whole[0], 3, "no_memory");
    expectDropped(whole[1], 4, "no_memory");
    expectDropped(whole[2], 0, "no_memory");
}

// ============================================================================================
// Receiver: limits and drops
// ============================================================================================

static void test_limit_exact_boundary_single_frame() {
    Outcome o = both(text(pattern(10)), maxOf(10));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)o.ev.size());
    expectText(o.ev[0], pattern(10));
    TEST_ASSERT_EQUAL_UINT32(11, (uint32_t)o.requests[0]);
}

static void test_limit_too_large_single_then_next_delivered() {
    Outcome o = both(cat({text(pattern(11)), text("ok")}), maxOf(10));
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)o.ev.size());
    expectDropped(o.ev[0], 11, "too_large");
    expectText(o.ev[1], "ok");
    TEST_ASSERT_EQUAL_INT(1, o.growCalls);  // the oversize message never asked for memory
}

static void test_limit_exact_boundary_fragmented() {
    Outcome o = both(cat({frame(false, 0x1, S("12345")), frame(true, 0x0, S("67890"))}), maxOf(10));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)o.ev.size());
    expectText(o.ev[0], "1234567890");
}

static void test_limit_too_large_fragmented() {
    Bytes in = cat({frame(false, 0x1, S("12345")), frame(false, 0x0, S("678901")), frame(false, 0x0, S("abc")),
                    frame(true, 0x0, S("de")), text("next")});
    Outcome o = both(in, maxOf(10));
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)o.ev.size());
    expectDropped(o.ev[0], 5 + 6 + 3 + 2, "too_large");  // counts skipped payload too
    expectText(o.ev[1], "next");
}

static void test_limit_too_large_on_first_fragment_of_many() {
    Bytes in = cat({frame(false, 0x1, S(pattern(20))), frame(true, 0x0, S("x")), text("n")});
    Outcome o = both(in, maxOf(10));
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)o.ev.size());
    expectDropped(o.ev[0], 21, "too_large");
    expectText(o.ev[1], "n");
    TEST_ASSERT_EQUAL_INT(1, o.growCalls);
}

static void test_limit_ping_during_discard() {
    Bytes in = cat({frame(false, 0x1, S(pattern(30))), frame(true, 0x9, S("pp")), frame(true, 0x0, S("zz")),
                    text("n")});
    Outcome o = both(in, maxOf(10));
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)o.ev.size());
    TEST_ASSERT_TRUE(o.ev[0].type == EventType::Ping);
    expectDropped(o.ev[1], 32, "too_large");
    expectText(o.ev[2], "n");
}

static void test_limit_huge_declared_length_requests_no_memory() {
    // 64-bit length of 1 TiB with a text opcode: dropped as too_large without any allocation.
    Bytes hdr = {0x81, 127, 0, 0, 1, 0, 0, 0, 0, 0};
    Bytes some(1000, 'x');
    CountingProvider p;
    {
        WsReceiver rx(&p, maxOf(1024));
        Event e;
        size_t u = rx.feed(hdr.data(), hdr.size(), e);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)hdr.size(), (uint32_t)u);
        TEST_ASSERT_TRUE(e.type == EventType::None);
        u = rx.feed(some.data(), some.size(), e);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)some.size(), (uint32_t)u);
        TEST_ASSERT_TRUE(e.type == EventType::None);
        TEST_ASSERT_EQUAL_UINT32(1000, (uint32_t)rx.messageBytesSoFar());  // skipped bytes counted
    }
    TEST_ASSERT_EQUAL_INT(0, p.growCalls);
    assertBalanced(p);
}

static void test_no_memory_on_first_grow() {
    ProviderCfg pc;
    pc.failCall = 1;
    Outcome o = both(cat({text("hello"), text("again")}), Limits(), pc);
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)o.ev.size());
    expectDropped(o.ev[0], 5, "no_memory");
    expectText(o.ev[1], "again");
}

static void test_no_memory_on_first_fragment() {
    ProviderCfg pc;
    pc.failCall = 1;
    Bytes in = cat({frame(false, 0x1, S("abc")), frame(false, 0x0, S("def")), frame(true, 0x0, S("g")), text("ok")});
    Outcome o = both(in, Limits(), pc);
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)o.ev.size());
    expectDropped(o.ev[0], 7, "no_memory");
    expectText(o.ev[1], "ok");
    TEST_ASSERT_EQUAL_INT(2, o.growCalls);  // no retries inside the dropped message
}

static void test_no_memory_on_later_fragment() {
    ProviderCfg pc;
    pc.failCall = 2;
    Bytes in = cat({frame(false, 0x1, S("abcd")), frame(false, 0x0, S("efgh")), frame(true, 0x0, S("ij")),
                    text("ok")});
    Outcome o = both(in, Limits(), pc);  // balanced check inside: the first buffer is released
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)o.ev.size());
    expectDropped(o.ev[0], 10, "no_memory");
    expectText(o.ev[1], "ok");
}

static void test_no_memory_threshold() {
    ProviderCfg pc;
    pc.failAbove = 100;
    Outcome o = both(cat({text(pattern(50)), text(pattern(200)), text(pattern(99))}), Limits(), pc);
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)o.ev.size());
    expectText(o.ev[0], pattern(50));
    expectDropped(o.ev[1], 200, "no_memory");
    expectText(o.ev[2], pattern(99));  // 99 + NUL = 100 is still granted
}

static void test_binary_single_dropped() {
    Outcome o = both(cat({frame(true, 0x2, Bytes{1, 2, 3, 4, 5}), text("after")}));
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)o.ev.size());
    expectDropped(o.ev[0], 5, "binary");
    expectText(o.ev[1], "after");
    TEST_ASSERT_EQUAL_INT(1, o.growCalls);  // binary never allocates
}

static void test_binary_empty_and_fragmented_with_ping() {
    Outcome o = both(cat({frame(true, 0x2, Bytes{}), frame(false, 0x2, Bytes{1, 2}), frame(true, 0x9, S("")),
                          frame(false, 0x0, Bytes{3}), frame(true, 0x0, Bytes{4, 5}), text("t")}));
    TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)o.ev.size());
    expectDropped(o.ev[0], 0, "binary");
    TEST_ASSERT_TRUE(o.ev[1].type == EventType::Ping);
    expectDropped(o.ev[2], 5, "binary");
    expectText(o.ev[3], "t");
}

static void test_limit_clamped_to_size_max_minus_one() {
    Limits lim;
    lim.maxMessageBytes = SIZE_MAX;
    // requests must be length+1 without wrapping; fragmented growth must not overflow either
    Outcome o = both(cat({text("abc"), frame(false, 0x1, S("de")), frame(true, 0x0, S("fgh"))}), lim);
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)o.ev.size());
    expectText(o.ev[0], "abc");
    expectText(o.ev[1], "defgh");
    TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)o.requests[0]);
    for (size_t r : o.requests) TEST_ASSERT_TRUE(r != 0 && r < (SIZE_MAX / 2));
}

static void test_limit_zero() {
    Outcome o = both(cat({text(""), text("a")}), maxOf(0));
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)o.ev.size());
    expectText(o.ev[0], "");
    expectDropped(o.ev[1], 1, "too_large");
}

// ============================================================================================
// Receiver: protocol errors
// ============================================================================================

static void expectOnlyError(const Bytes &bad, const char *reason) {
    // The error frame is followed by a valid text frame; after the error everything is ignored.
    Outcome o = both(cat({bad, text("ignored"), text("also ignored")}));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, (uint32_t)o.ev.size(), reason);
    expectProtoError(o.ev[0], reason);
}

static void test_err_masked_server_frame() {
    expectOnlyError(Bytes{0x81, 0x85, 1, 2, 3, 4, 'h', 'e', 'l', 'l', 'o'}, "masked_frame");
    expectOnlyError(Bytes{0x89, 0x80, 1, 2, 3, 4}, "masked_frame");  // masked empty ping
}

static void test_err_reserved_bits() {
    expectOnlyError(frame(true, 0x1, S("x"), 0x40), "reserved_bits");
    expectOnlyError(frame(true, 0x1, S("x"), 0x20), "reserved_bits");
    expectOnlyError(frame(true, 0x1, S("x"), 0x10), "reserved_bits");
    expectOnlyError(frame(true, 0x9, S(""), 0x40), "reserved_bits");
}

static void test_err_bad_opcode() {
    const uint8_t ops[] = {0x3, 0x4, 0x5, 0x6, 0x7, 0xB, 0xC, 0xD, 0xE, 0xF};
    for (uint8_t op : ops) expectOnlyError(frame(true, op, S("")), "bad_opcode");
}

static void test_err_unexpected_continuation() {
    expectOnlyError(frame(true, 0x0, S("x")), "unexpected_continuation");
    expectOnlyError(frame(false, 0x0, S("x")), "unexpected_continuation");
    // after a complete message there is nothing to continue
    Outcome o = both(cat({text("a"), frame(true, 0x0, S("x"))}));
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)o.ev.size());
    expectText(o.ev[0], "a");
    expectProtoError(o.ev[1], "unexpected_continuation");
}

static void test_err_expected_continuation() {
    Outcome o = both(cat({frame(false, 0x1, S("ab")), text("new")}));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)o.ev.size());
    expectProtoError(o.ev[0], "expected_continuation");
    o = both(cat({frame(false, 0x1, S("ab")), frame(true, 0x2, S("bin"))}));
    expectProtoError(o.ev[0], "expected_continuation");
    // also while discarding a binary message
    o = both(cat({frame(false, 0x2, S("ab")), frame(true, 0x1, S("new"))}));
    expectProtoError(o.ev[0], "expected_continuation");
}

static void test_err_bad_length() {
    Bytes f = {0x81, 127, 0x80, 0, 0, 0, 0, 0, 0, 0};
    expectOnlyError(f, "bad_length");
    Bytes g = {0x81, 127, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    expectOnlyError(g, "bad_length");
}

static void test_err_bad_control() {
    expectOnlyError(Bytes{0x09, 0x00}, "bad_control");                  // fragmented ping
    expectOnlyError(Bytes{0x08, 0x00}, "bad_control");                  // fragmented close
    expectOnlyError(Bytes{0x8A, 126, 0, 126}, "bad_control");          // pong with 16-bit length
    expectOnlyError(Bytes{0x89, 126}, "bad_control");                   // detected on the length byte
    expectOnlyError(Bytes{0x89, 127}, "bad_control");
    expectOnlyError(frame(true, 0x8, Bytes{0x03}), "bad_control");     // 1-byte close body
}

static void test_err_buffer_released_and_dead_until_reset() {
    CountingProvider p;
    WsReceiver rx(&p);
    // half a fragmented message is in the buffer, then a protocol error
    Bytes a = cat({frame(false, 0x1, S("abc")), frame(false, 0x1, S("oops"))});
    Event e;
    size_t used = 0;
    while (used < a.size()) {
        const size_t n = rx.feed(a.data() + used, a.size() - used, e);
        used += n;
        if (e.type != EventType::None) break;
    }
    TEST_ASSERT_TRUE(e.type == EventType::ProtocolError);
    TEST_ASSERT_EQUAL_STRING("expected_continuation", e.reason);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)p.live.size());  // released right away

    // dead: consumes everything, returns None
    Bytes b = text("hello");
    for (int i = 0; i < 3; ++i) {
        Event e2;
        e2.type = EventType::Text;  // feed() must overwrite
        TEST_ASSERT_EQUAL_UINT32((uint32_t)b.size(), (uint32_t)rx.feed(b.data(), b.size(), e2));
        TEST_ASSERT_TRUE(e2.type == EventType::None);
    }
    // reset revives
    rx.reset();
    Event e3;
    TEST_ASSERT_EQUAL_UINT32((uint32_t)b.size(), (uint32_t)rx.feed(b.data(), b.size(), e3));
    TEST_ASSERT_TRUE(e3.type == EventType::Text);
    TEST_ASSERT_EQUAL_STRING("hello", (const char *)e3.data);
}

// ============================================================================================
// Receiver: reset, destructor, feed() contract
// ============================================================================================

static void test_reset_mid_message_releases_and_restarts() {
    CountingProvider p;
    WsReceiver rx(&p);
    // first fragment complete, second fragment half received
    Bytes in = cat({frame(false, 0x1, S("abcd")), frame(false, 0x0, S("efghij"))});
    in.resize(in.size() - 3);
    Event e;
    size_t used = rx.feed(in.data(), in.size(), e);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)in.size(), (uint32_t)used);
    TEST_ASSERT_TRUE(e.type == EventType::None);
    TEST_ASSERT_TRUE(p.live.size() == 1);
    TEST_ASSERT_TRUE(rx.messageBytesSoFar() > 0);
    rx.reset();
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)p.live.size());
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)rx.messageBytesSoFar());
    assertBalanced(p);
    // a fresh message after reset is not mixed with the old state (no continuation expected)
    Bytes t = text("fresh");
    used = rx.feed(t.data(), t.size(), e);
    TEST_ASSERT_TRUE(e.type == EventType::Text);
    TEST_ASSERT_EQUAL_STRING("fresh", (const char *)e.data);
    rx.reset();  // reset right after a delivered Text frees its buffer too
    assertBalanced(p);
}

static void test_reset_mid_header_and_mid_skip() {
    CountingProvider p;
    WsReceiver rx(&p, maxOf(4));
    Event e;
    const uint8_t half[] = {0x81, 126, 0x01};  // 16-bit length incomplete
    rx.feed(half, sizeof(half), e);
    rx.reset();
    Bytes drop = frame(true, 0x1, S("0123456789"));
    drop.resize(8);  // in the middle of a discarded message
    rx.feed(drop.data(), drop.size(), e);
    TEST_ASSERT_TRUE(e.type == EventType::None);
    TEST_ASSERT_TRUE(rx.messageBytesSoFar() > 0);
    rx.reset();
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)rx.messageBytesSoFar());
    Bytes t = text("ok");
    rx.feed(t.data(), t.size(), e);
    TEST_ASSERT_TRUE(e.type == EventType::Text);
    TEST_ASSERT_EQUAL_STRING("ok", (const char *)e.data);
}

static void test_destructor_mid_message_releases() {
    CountingProvider p;
    {
        WsReceiver rx(&p);
        Bytes in = cat({frame(false, 0x1, S("abcd")), frame(false, 0x0, S("ef"))});
        in.resize(in.size() - 1);
        Event e;
        rx.feed(in.data(), in.size(), e);
        TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)p.live.size());
    }
    assertBalanced(p);
    // destructor right after a delivered Text
    {
        WsReceiver rx(&p);
        Bytes t = text("zzz");
        Event e;
        rx.feed(t.data(), t.size(), e);
        TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)p.live.size());
    }
    assertBalanced(p);
}

static void test_feed_one_event_per_call_and_progress() {
    Bytes in = cat({text("one"), frame(true, 0x9, S("p")), text("three")});
    CountingProvider p;
    WsReceiver rx(&p);
    size_t off = 0;
    int events = 0;
    while (off < in.size()) {
        Event e;
        size_t used = rx.feed(in.data() + off, in.size() - off, e);
        TEST_ASSERT_TRUE(used > 0);
        TEST_ASSERT_TRUE(e.type != EventType::None);  // the buffer holds only complete frames
        off += used;
        ++events;
    }
    TEST_ASSERT_EQUAL_INT(3, events);
}

static void test_feed_stops_after_event_leaving_rest() {
    Bytes a = text("one"), b = text("two");
    Bytes in = cat({a, b});
    CountingProvider p;
    WsReceiver rx(&p);
    Event e;
    size_t used = rx.feed(in.data(), in.size(), e);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)a.size(), (uint32_t)used);
    TEST_ASSERT_TRUE(e.type == EventType::Text);
}

static void test_feed_header_only_bytes_progress() {
    // A header split across calls consumes its bytes with a None event each time.
    Bytes f = text(pattern(300));
    CountingProvider p;
    WsReceiver rx(&p);
    for (size_t i = 0; i < 3; ++i) {
        Event e;
        size_t used = rx.feed(f.data() + i, 1, e);
        TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)used);
        TEST_ASSERT_TRUE(e.type == EventType::None);
    }
}

static void test_message_bytes_so_far() {
    CountingProvider p;
    WsReceiver rx(&p, maxOf(6));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)rx.messageBytesSoFar());
    Bytes a = cat({frame(false, 0x1, S("abcd")), frame(false, 0x0, S("ef"))});
    Event e;
    rx.feed(a.data(), a.size(), e);
    TEST_ASSERT_EQUAL_UINT32(6, (uint32_t)rx.messageBytesSoFar());
    Bytes b = frame(false, 0x0, S("ghij"));  // goes over the limit; skipped bytes still counted
    rx.feed(b.data(), b.size(), e);
    TEST_ASSERT_EQUAL_UINT32(10, (uint32_t)rx.messageBytesSoFar());
    Bytes c = frame(true, 0x0, S("k"));
    rx.feed(c.data(), c.size(), e);
    TEST_ASSERT_TRUE(e.type == EventType::Dropped);
    TEST_ASSERT_EQUAL_UINT32(11, (uint32_t)e.length);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)rx.messageBytesSoFar());
}

static void test_integration_handshake_leftover_to_receiver() {
    const std::string hdr = okResponse();
    const Bytes first = text("{\"type\":\"auth_required\"}");
    std::string all = hdr;
    all.append((const char *)first.data(), first.size());
    UpgradeResult r = parseUpgradeResponse(all.data(), all.size(), kRfcKey);
    TEST_ASSERT_TRUE(r.status == UpgradeResult::Status::Ok);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)hdr.size(), (uint32_t)r.headerBytes);
    Bytes rest(all.begin() + (long)r.headerBytes, all.end());
    Outcome o = both(rest);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)o.ev.size());
    expectText(o.ev[0], "{\"type\":\"auth_required\"}");
}

int main(int, char **) {
    UNITY_BEGIN();
    // handshake
    RUN_TEST(test_accept_rfc_example);
    RUN_TEST(test_key_from_fixed_rng);
    RUN_TEST(test_upgrade_request_ws_80);
    RUN_TEST(test_upgrade_request_ws_8123);
    RUN_TEST(test_upgrade_request_wss_443);
    RUN_TEST(test_upgrade_request_port_defaults_are_per_scheme);
    RUN_TEST(test_upgrade_request_prefix_path_and_no_origin);
    RUN_TEST(test_response_ok);
    RUN_TEST(test_response_mixed_case);
    RUN_TEST(test_response_connection_list);
    RUN_TEST(test_response_connection_upgradefoo_rejected);
    RUN_TEST(test_response_incomplete_and_split_terminator);
    RUN_TEST(test_response_bad_accept);
    RUN_TEST(test_response_missing_accept_is_bad_accept);
    RUN_TEST(test_response_missing_upgrade_or_connection_is_bad_response);
    RUN_TEST(test_response_malformed_header_line);
    RUN_TEST(test_response_http_errors);
    RUN_TEST(test_response_garbage_status_line);
    RUN_TEST(test_response_4096_boundary);
    // encoder
    RUN_TEST(test_encode_lengths);
    RUN_TEST(test_encode_64bit_length_bytes);
    RUN_TEST(test_encode_rfc_hello);
    RUN_TEST(test_encode_other_opcodes);
    RUN_TEST(test_encode_close_1000);
    // receiver happy paths
    RUN_TEST(test_rx_text_sizes);
    RUN_TEST(test_rx_text_with_embedded_nul);
    RUN_TEST(test_rx_two_messages);
    RUN_TEST(test_rx_many_messages_and_empty);
    RUN_TEST(test_rx_fragmented_with_ping_between);
    RUN_TEST(test_rx_fragmented_large_content);
    RUN_TEST(test_rx_ping_pong_payloads);
    RUN_TEST(test_rx_close_with_code_and_reason);
    RUN_TEST(test_rx_close_code_only);
    RUN_TEST(test_rx_close_without_body_is_1005);
    RUN_TEST(test_rx_close_max_reason);
    RUN_TEST(test_rx_close_one_byte_body_is_protocol_error);
    RUN_TEST(test_rx_events_before_and_after_close);
    RUN_TEST(test_rx_64bit_length_form_for_small_payload);
    // provider contract
    RUN_TEST(test_provider_single_frame_requests_len_plus_one);
    RUN_TEST(test_provider_first_grow_has_null_current_and_release_after_next_feed);
    RUN_TEST(test_provider_balanced_after_many_messages);
    RUN_TEST(test_provider_geometric_growth_for_tiny_fragments);
    RUN_TEST(test_provider_zero_length_continuations_do_not_grow);
    RUN_TEST(test_provider_null_provider_drops_with_no_memory);
    // limits and drops
    RUN_TEST(test_limit_exact_boundary_single_frame);
    RUN_TEST(test_limit_too_large_single_then_next_delivered);
    RUN_TEST(test_limit_exact_boundary_fragmented);
    RUN_TEST(test_limit_too_large_fragmented);
    RUN_TEST(test_limit_too_large_on_first_fragment_of_many);
    RUN_TEST(test_limit_ping_during_discard);
    RUN_TEST(test_limit_huge_declared_length_requests_no_memory);
    RUN_TEST(test_no_memory_on_first_grow);
    RUN_TEST(test_no_memory_on_first_fragment);
    RUN_TEST(test_no_memory_on_later_fragment);
    RUN_TEST(test_no_memory_threshold);
    RUN_TEST(test_binary_single_dropped);
    RUN_TEST(test_binary_empty_and_fragmented_with_ping);
    RUN_TEST(test_limit_clamped_to_size_max_minus_one);
    RUN_TEST(test_limit_zero);
    // protocol errors
    RUN_TEST(test_err_masked_server_frame);
    RUN_TEST(test_err_reserved_bits);
    RUN_TEST(test_err_bad_opcode);
    RUN_TEST(test_err_unexpected_continuation);
    RUN_TEST(test_err_expected_continuation);
    RUN_TEST(test_err_bad_length);
    RUN_TEST(test_err_bad_control);
    RUN_TEST(test_err_buffer_released_and_dead_until_reset);
    // reset / destructor / contract
    RUN_TEST(test_reset_mid_message_releases_and_restarts);
    RUN_TEST(test_reset_mid_header_and_mid_skip);
    RUN_TEST(test_destructor_mid_message_releases);
    RUN_TEST(test_feed_one_event_per_call_and_progress);
    RUN_TEST(test_feed_stops_after_event_leaving_rest);
    RUN_TEST(test_feed_header_only_bytes_progress);
    RUN_TEST(test_message_bytes_so_far);
    RUN_TEST(test_integration_handshake_leftover_to_receiver);
    return UNITY_END();
}
