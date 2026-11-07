/* =====================================================================
 *  Square Browser Engine — square_h2.h
 *  Real HTTP/2 (RFC 7540/7541) client: ALPN-negotiated, connection
 *  preface, SETTINGS, HEADERS/DATA framing, flow control, HPACK with
 *  static+dynamic tables and Huffman coding (RFC 7541 Appendix B).
 *  Single active stream per connection (browser fetches are sequential);
 *  pushes are refused; connection + stream windows are kept open.
 * ===================================================================== */
#pragma once

#include "square_internal.h"

#include <cstring>
#include <string>
#include <vector>

/* included inside namespace sqrl */

/* ---------------- HPACK: static table (RFC 7541 Appendix A) ---------------- */
struct HpackStaticEntry {
  const char* name;
  const char* value;
};
inline const HpackStaticEntry kHpackStatic[62] = {
    {"", ""},                                          /* index 0 unused */
    {":authority", ""},                                /* 1  */
    {":method", "GET"},                                /* 2  */
    {":method", "POST"},                               /* 3  */
    {":path", "/"},                                    /* 4  */
    {":path", "/index.html"},                          /* 5  */
    {":scheme", "http"},                               /* 6  */
    {":scheme", "https"},                              /* 7  */
    {":status", "200"},                                /* 8  */
    {":status", "204"},                                /* 9  */
    {":status", "206"},                                /* 10 */
    {":status", "304"},                                /* 11 */
    {":status", "400"},                                /* 12 */
    {":status", "404"},                                /* 13 */
    {":status", "500"},                                /* 14 */
    {"accept-charset", ""},                            /* 15 */
    {"accept-encoding", "gzip, deflate"},              /* 16 */
    {"accept-language", ""},                           /* 17 */
    {"accept-ranges", ""},                             /* 18 */
    {"accept", ""},                                    /* 19 */
    {"access-control-allow-origin", ""},               /* 20 */
    {"age", ""},                                       /* 21 */
    {"allow", ""},                                     /* 22 */
    {"authorization", ""},                             /* 23 */
    {"cache-control", ""},                             /* 24 */
    {"content-disposition", ""},                       /* 25 */
    {"content-encoding", ""},                          /* 26 */
    {"content-language", ""},                          /* 27 */
    {"content-length", ""},                            /* 28 */
    {"content-location", ""},                          /* 29 */
    {"content-range", ""},                             /* 30 */
    {"content-type", ""},                              /* 31 */
    {"cookie", ""},                                    /* 32 */
    {"date", ""},                                      /* 33 */
    {"etag", ""},                                      /* 34 */
    {"expect", ""},                                    /* 35 */
    {"expires", ""},                                   /* 36 */
    {"from", ""},                                      /* 37 */
    {"host", ""},                                      /* 38 */
    {"if-match", ""},                                  /* 39 */
    {"if-modified-since", ""},                         /* 40 */
    {"if-none-match", ""},                             /* 41 */
    {"if-range", ""},                                  /* 42 */
    {"if-unmodified-since", ""},                       /* 43 */
    {"last-modified", ""},                             /* 44 */
    {"link", ""},                                      /* 45 */
    {"location", ""},                                  /* 46 */
    {"max-forwards", ""},                              /* 47 */
    {"proxy-authenticate", ""},                        /* 48 */
    {"proxy-authorization", ""},                       /* 49 */
    {"range", ""},                                     /* 50 */
    {"referer", ""},                                   /* 51 */
    {"refresh", ""},                                   /* 52 */
    {"retry-after", ""},                               /* 53 */
    {"server", ""},                                    /* 54 */
    {"set-cookie", ""},                                /* 55 */
    {"strict-transport-security", ""},                 /* 56 */
    {"transfer-encoding", ""},                         /* 57 */
    {"user-agent", ""},                                /* 58 */
    {"vary", ""},                                      /* 59 */
    {"via", ""},                                       /* 60 */
    {"www-authenticate", ""},                          /* 61 */
};

/* ---------------- HPACK: dynamic table ---------------- */
struct HpackDynTable {
  std::vector<std::pair<std::string, std::string>> ring; /* front = newest */
  size_t bytes = 0;
  size_t max_bytes = 4096;

  void add(const std::string& n, const std::string& v) {
    ring.insert(ring.begin(), {n, v});
    bytes += n.size() + v.size() + 32;
    while (bytes > max_bytes && !ring.empty()) {
      auto& back = ring.back();
      bytes -= back.first.size() + back.second.size() + 32;
      ring.pop_back();
    }
    if (bytes > max_bytes) { /* single huge entry */
      ring.clear();
      bytes = 0;
    }
  }
  bool lookup(size_t index, std::string* n, std::string* v) const {
    if (index == 0) return false;
    if (index < 62) {
      *n = kHpackStatic[index].name;
      *v = kHpackStatic[index].value;
      return true;
    }
    size_t k = index - 62;
    if (k >= ring.size()) return false;
    *n = ring[k].first;
    *v = ring[k].second;
    return true;
  }
};

/* ---------------- HPACK: primitives ---------------- */
inline bool hpack_read_int(const uint8_t* d, size_t n, size_t& pos, uint8_t prefix_bits,
                           uint32_t* out) {
  if (pos >= n) return false;
  uint8_t mask = (uint8_t)((1u << prefix_bits) - 1);
  uint32_t v = d[pos] & mask;
  pos++;
  if (v < mask) {
    *out = v;
    return true;
  }
  uint32_t m = 0;
  for (;;) {
    if (pos >= n) return false;
    uint8_t b = d[pos++];
    v += (uint32_t)(b & 127) << m;
    if (!(b & 128)) break;
    m += 7;
    if (m > 28) return false;
  }
  *out = v;
  return true;
}

inline void hpack_write_int(std::vector<uint8_t>& o, uint8_t first_byte, uint8_t prefix_bits,
                            uint32_t v) {
  uint8_t mask = (uint8_t)((1u << prefix_bits) - 1);
  if (v < mask) {
    o.push_back(first_byte | (uint8_t)v);
    return;
  }
  o.push_back(first_byte | mask);
  v -= mask;
  while (v >= 128) {
    o.push_back((uint8_t)(v & 127) | 128);
    v >>= 7;
  }
  o.push_back((uint8_t)v);
}

/* Huffman decode via canonical table (kHuff in hpack_huff.h) */
#include "hpack_huff.h"

struct HuffNode {
  int sym = -1; /* -1 internal */
  HuffNode* kid[2] = {nullptr, nullptr};
};
inline HuffNode* huff_tree() {
  static HuffNode* root = nullptr;
  static bool built = false;
  if (!built) {
    built = true;
    root = new HuffNode();
    for (int s = 0; s < 257; s++) {
      HuffNode* cur = root;
      for (int bit = kHuff[s].bits - 1; bit >= 0; bit--) {
        int b = (kHuff[s].code >> bit) & 1;
        if (!cur->kid[b]) cur->kid[b] = new HuffNode();
        cur = cur->kid[b];
      }
      cur->sym = s;
    }
  }
  return root;
}

inline bool huffman_decode(const uint8_t* d, size_t n, std::string* out) {
  HuffNode* root = huff_tree();
  HuffNode* cur = root;
  int leftover = 0; /* bits consumed since the last complete symbol */
  for (size_t i = 0; i < n; i++) {
    uint8_t byte = d[i];
    for (int b = 7; b >= 0; b--) {
      int bit = (byte >> b) & 1;
      cur = cur->kid[bit];
      if (!cur) return false;
      if (cur->sym >= 0) {
        if (cur->sym == 256) return false; /* EOS inside string = error */
        out->push_back((char)cur->sym);
        cur = root;
        leftover = 0;
      } else {
        leftover++;
      }
    }
  }
  if (cur == root) return true;          /* byte-aligned end */
  /* padding must be a prefix of EOS (all ones), at most 7 bits */
  if (leftover > 7 || n == 0) return false;
  /* verify each padding bit (low `leftover` bits of the final byte) is 1 */
  uint8_t last = d[n - 1];
  for (int k = 0; k < leftover; k++)
    if (!((last >> k) & 1)) return false;
  return true;
}

inline void huffman_encode(const std::string& s, std::vector<uint8_t>* out) {
  uint64_t acc = 0;
  int nbits = 0;
  for (unsigned char c : s) {
    const HuffEntry& e = kHuff[c];
    acc = (acc << e.bits) | e.code;
    nbits += e.bits;
    while (nbits >= 8) {
      nbits -= 8;
      out->push_back((uint8_t)(acc >> nbits));
    }
  }
  if (nbits > 0) {
    acc = (acc << (8 - nbits)) | ((1u << (8 - nbits)) - 1); /* pad with 1s (EOS) */
    out->push_back((uint8_t)acc);
  }
}

/* decode a header block into name/value pairs */
inline bool hpack_decode_block(const uint8_t* d, size_t n, HpackDynTable& dyn,
                               std::vector<std::pair<std::string, std::string>>* out) {
  size_t pos = 0;
  while (pos < n) {
    uint8_t b = d[pos];
    if (b & 128) { /* 1xxxxxxx: indexed header field */
      uint32_t idx;
      if (!hpack_read_int(d, n, pos, 7, &idx)) return false;
      std::string nm, vl;
      if (!dyn.lookup(idx, &nm, &vl)) return false;
      out->push_back({nm, vl});
    } else if ((b & 192) == 64) { /* 01xxxxxx: literal w/ incremental indexing */
      uint32_t idx;
      if (!hpack_read_int(d, n, pos, 6, &idx)) return false;
      std::string nm, vl;
      if (idx) {
        if (!dyn.lookup(idx, &nm, &vl)) return false;
      } else {
        if (pos >= n) return false;
        bool huff = (d[pos] & 128) != 0;
        uint32_t len;
        if (!hpack_read_int(d, n, pos, 7, &len)) return false;
        if (pos + len > n) return false;
        if (huff) {
          if (!huffman_decode(d + pos, len, &nm)) return false;
        } else {
          nm.assign((const char*)d + pos, len);
        }
        pos += len;
      }
      if (pos >= n) return false;
      bool huff2 = (d[pos] & 128) != 0;
      uint32_t vlen;
      if (!hpack_read_int(d, n, pos, 7, &vlen)) return false;
      if (pos + vlen > n) return false;
      if (huff2) {
        if (!huffman_decode(d + pos, vlen, &vl)) return false;
      } else {
        vl.assign((const char*)d + pos, vlen);
      }
      pos += vlen;
      dyn.add(nm, vl);
      out->push_back({nm, vl});
    } else if ((b & 224) == 32) { /* 001xxxxx: dynamic table size update */
      uint32_t sz;
      if (!hpack_read_int(d, n, pos, 5, &sz)) return false;
      dyn.max_bytes = sz;
      dyn.ring.clear();
      dyn.bytes = 0;
    } else { /* 0000xxxx / 0001xxxx: literal without indexing / never indexed */
      uint32_t idx;
      if (!hpack_read_int(d, n, pos, 4, &idx)) return false;
      std::string nm, vl;
      if (idx) {
        if (!dyn.lookup(idx, &nm, &vl)) return false;
      } else {
        if (pos >= n) return false;
        bool huff = (d[pos] & 128) != 0;
        uint32_t len;
        if (!hpack_read_int(d, n, pos, 7, &len)) return false;
        if (pos + len > n) return false;
        if (huff) {
          if (!huffman_decode(d + pos, len, &nm)) return false;
        } else {
          nm.assign((const char*)d + pos, len);
        }
        pos += len;
      }
      if (pos >= n) return false;
      bool huff = (d[pos] & 128) != 0;
      uint32_t vlen;
      if (!hpack_read_int(d, n, pos, 7, &vlen)) return false;
      if (pos + vlen > n) return false;
      if (huff) {
        if (!huffman_decode(d + pos, vlen, &vl)) return false;
      } else {
        vl.assign((const char*)d + pos, vlen);
      }
      pos += vlen;
      out->push_back({nm, vl});
    }
  }
  return true;
}

/* encode request headers (HPACK) */
inline void hpack_encode_block(const std::vector<std::pair<std::string, std::string>>& headers,
                               std::vector<uint8_t>* out) {
  for (auto& kv : headers) {
    const std::string& n = kv.first;
    const std::string& v = kv.second;
    /* indexed representations for the static-table exact matches */
    if (n == ":method" && v == "GET") {
      out->push_back(0x82);
      continue;
    }
    if (n == ":method" && v == "POST") {
      out->push_back(0x83);
      continue;
    }
    if (n == ":scheme" && v == "https") {
      out->push_back(0x87);
      continue;
    }
    if (n == ":scheme" && v == "http") {
      out->push_back(0x86);
      continue;
    }
    /* literal without indexing, indexed name when available */
    int name_idx = -1;
    for (int i = 1; i < 62; i++) {
      if (n == kHpackStatic[i].name && kHpackStatic[i].value[0] == 0) {
        name_idx = i;
        break;
      }
    }
    if (name_idx > 0) {
      hpack_write_int(*out, 0x00, 4, (uint32_t)name_idx); /* 0000 + idx */
    } else {
      out->push_back(0x00);
      hpack_write_int(*out, 0x00, 7, (uint32_t)n.size());
      out->insert(out->end(), n.begin(), n.end());
    }
    hpack_write_int(*out, 0x00, 7, (uint32_t)v.size()); /* H=0 raw */
    out->insert(out->end(), v.begin(), v.end());
  }
}

/* ---------------- frames ---------------- */
enum : uint8_t {
  H2_DATA = 0,
  H2_HEADERS = 1,
  H2_PRIORITY = 2,
  H2_RST_STREAM = 3,
  H2_SETTINGS = 4,
  H2_PUSH_PROMISE = 5,
  H2_PING = 6,
  H2_GOAWAY = 7,
  H2_WINDOW_UPDATE = 8,
  H2_CONTINUATION = 9,
};
enum : uint8_t { F_END_STREAM = 0x1, F_END_HEADERS = 0x4, F_PADDED = 0x8, F_PRIORITY = 0x20 };
enum : uint8_t { FACK = 0x1 };

inline const char* h2_preface() { return "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"; }

