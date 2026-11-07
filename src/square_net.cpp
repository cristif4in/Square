/* =====================================================================
 *  Square Browser Engine — square_net.cpp
 *  HTTP/1.1 client (keep-alive-less), TLS via OpenSSL (POSIX) or
 *  Schannel (Windows), gzip/zlib/deflate decoding, RFC3986 URL utils,
 *  data: URLs, cookies, worker thread pool.
 * ===================================================================== */
#include "square_internal.h"

#include <chrono>
#include <condition_variable>
#include <cctype>
#include <deque>
#include <functional>
#include <queue>
#include <stdarg.h>
#include <zlib.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  define SECURITY_WIN32
#  include <security.h>
#  include <schannel.h>
#  ifndef SECBUFFER_EXTRA
#    define SECBUFFER_EXTRA 5
#  endif
#  ifndef SECBUFFER_STREAM_TRAILER
#    define SECBUFFER_STREAM_TRAILER 6
#  endif
#  ifndef SECBUFFER_STREAM_HEADER
#    define SECBUFFER_STREAM_HEADER 7
#  endif
#else
#  include <fcntl.h>
#  include <netdb.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
#  include <openssl/err.h>
#  include <openssl/ssl.h>
#  include <openssl/x509v3.h>
#endif

namespace sqrl {

/* ============================ ThreadPool ============================ */
struct ThreadPool::Impl {
  std::vector<std::thread> workers;
  std::queue<std::function<void()>> jobs;
  std::mutex mtx;
  std::condition_variable cv_work, cv_idle;
  int active = 0;
  bool quitting = false;
};

ThreadPool::ThreadPool(int n_threads) {
  if (n_threads < 1) n_threads = 1;
  impl_ = new Impl();
  for (int i = 0; i < n_threads; i++) {
    impl_->workers.emplace_back([this]() {
      for (;;) {
        std::function<void()> job;
        {
          std::unique_lock<std::mutex> lk(impl_->mtx);
          impl_->cv_work.wait(lk, [&] { return impl_->quitting || !impl_->jobs.empty(); });
          if (impl_->quitting && impl_->jobs.empty()) return;
          job = std::move(impl_->jobs.front());
          impl_->jobs.pop();
          impl_->active++;
        }
        job();
        {
          std::unique_lock<std::mutex> lk(impl_->mtx);
          impl_->active--;
          if (impl_->jobs.empty() && impl_->active == 0) impl_->cv_idle.notify_all();
        }
      }
    });
  }
}
ThreadPool::~ThreadPool() { wait_idle(); drain(); }
void ThreadPool::submit(std::function<void()> f) {
  if (!impl_) return;
  {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    if (impl_->quitting) return;
    impl_->jobs.push(std::move(f));
  }
  impl_->cv_work.notify_one();
}
void ThreadPool::wait_idle() {
  if (!impl_) return;
  std::unique_lock<std::mutex> lk(impl_->mtx);
  impl_->cv_idle.wait(lk, [&] { return impl_->jobs.empty() && impl_->active == 0; });
}
int ThreadPool::thread_count() const { return impl_ ? (int)impl_->workers.size() : 0; }
void ThreadPool::drain() {
  if (!impl_) return;
  {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    std::queue<std::function<void()>> empty;
    impl_->jobs.swap(empty);
    impl_->quitting = true;
  }
  impl_->cv_work.notify_all();
  for (auto& t : impl_->workers) if (t.joinable()) t.join();
  impl_->workers.clear();
  delete impl_;
  impl_ = nullptr;
}

/* ============================ string utils ============================ */
static std::string lower_copy(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}
static bool ieq(const std::string& a, const char* b) {
  size_t n = strlen(b);
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; i++) if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
  return true;
}
static std::string trim_copy(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && isspace((unsigned char)s[a])) a++;
  while (b > a && isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}

/* ============================ URL utils ============================ */
bool url_split(const std::string& url, std::string* scheme, std::string* host,
               std::string* port, std::string* path_query) {
  size_t p = url.find("://");
  if (p == std::string::npos) return false;
  std::string sch = lower_copy(url.substr(0, p));
  size_t hp = p + 3, e = hp;
  while (e < url.size() && url[e] != '/' && url[e] != '?' && url[e] != '#') e++;
  std::string hostport = url.substr(hp, e - hp);
  /* file:///path has no host — that is valid */
  if (sch == "file") {
    /* hostport may be "" (file:///path) or "localhost" */
    std::string h = hostport, po;
    std::string pq = url.substr(e);
    size_t frag = pq.find('#');
    if (frag != std::string::npos) pq = pq.substr(0, frag);
    if (pq.empty() || pq[0] == '?') pq = "/" + pq;
    if (scheme) *scheme = sch;
    if (host) *host = lower_copy(h);
    if (port) *port = po;
    if (path_query) *path_query = pq;
    return true;
  }
  /* strip userinfo */
  size_t at = hostport.rfind('@');
  if (at != std::string::npos) hostport = hostport.substr(at + 1);
  std::string h, po;
  if (!hostport.empty() && hostport[0] == '[') { /* IPv6 */
    size_t cb = hostport.find(']');
    if (cb == std::string::npos) return false;
    h = hostport.substr(0, cb + 1);
    if (cb + 1 < hostport.size() && hostport[cb + 1] == ':') po = hostport.substr(cb + 2);
  } else {
    size_t c = hostport.rfind(':');
    if (c != std::string::npos) { h = hostport.substr(0, c); po = hostport.substr(c + 1); }
    else h = hostport;
  }
  if (h.empty()) return false;
  if (po.empty()) {
    if (sch == "http") po = "80";
    else if (sch == "https") po = "443";
  }
  std::string pq = url.substr(e);
  size_t frag = pq.find('#');
  if (frag != std::string::npos) pq = pq.substr(0, frag);
  if (pq.empty() || pq[0] == '?' ) pq = "/" + pq;
  if (scheme) *scheme = sch;
  if (host) *host = lower_copy(h);
  if (port) *port = po;
  if (path_query) *path_query = pq;
  return true;
}

static void normalize_path(std::vector<std::string>& segs) {
  for (size_t i = 0; i < segs.size();) {
    if (segs[i] == "." || segs[i] == "%2e" || segs[i] == "%2E") { segs.erase(segs.begin() + i); }
    else if (segs[i] == ".." || segs[i] == "%2e." || segs[i] == ".%2e") {
      if (i > 0 && segs[i - 1] != "..") { segs.erase(segs.begin() + i - 1, segs.begin() + i + 1); i--; }
      else { segs.erase(segs.begin() + i); }
    } else i++;
  }
}

std::string url_resolve(const std::string& base, const std::string& rel) {
  std::string r = trim_copy(rel);
  /* strip fragment from rel if it contains one before ?-ish handling (fragments not fetched) */
  size_t h0 = r.find('#');
  if (h0 != std::string::npos && r.find('/') != std::string::npos && r.rfind('/') < h0) {
    /* keep as is; fragment removed below for results */
  }
  std::string bs, bh, bp, bq;
  bool has_base = url_split(base, &bs, &bh, &bp, &bq);
  if (r.empty()) { std::string out = base; size_t f = out.find('#'); if (f != std::string::npos) out = out.substr(0, f); return out; }
  if (!has_base) return r;
  if (r.find("://") != std::string::npos) { size_t f = r.find('#'); return f == std::string::npos ? r : r.substr(0, f); }
  if (r.rfind("//", 0) == 0) { std::string out = bs + ":" + r; size_t f = out.find('#'); if (f != std::string::npos) out = out.substr(0, f); return out; }
  if (r[0] == '#') return base;
  if (r.rfind("data:", 0) == 0 || r.rfind("about:", 0) == 0 || r.rfind("javascript:", 0) == 0 || r.rfind("blob:", 0) == 0) return r;

  std::string dir = bq;
  size_t q = dir.find('?');
  std::string base_path = q == std::string::npos ? dir : dir.substr(0, q);
  std::string base_query = q == std::string::npos ? "" : dir.substr(q);
  size_t slash = base_path.rfind('/');
  std::string base_dir = slash == std::string::npos ? "/" : base_path.substr(0, slash + 1);

  std::string out_path, out_query;
  if (r[0] == '/') { out_path = r; size_t qq = out_path.find('?'); if (qq != std::string::npos) { out_query = out_path.substr(qq); out_path = out_path.substr(0, qq); } }
  else if (r[0] == '?') { out_path = base_path; out_query = r; }
  else {
    std::string merged = base_dir + r;
    size_t qq = merged.find('?');
    if (qq != std::string::npos) { out_query = merged.substr(qq); merged = merged.substr(0, qq); }
    out_path = merged;
  }
  /* normalize segments */
  bool trailing_slash = !out_path.empty() && out_path.back() == '/';
  std::vector<std::string> segs;
  size_t i = 0;
  while (i < out_path.size()) {
    size_t e = out_path.find('/', i);
    if (e == std::string::npos) e = out_path.size();
    if (e > i) segs.push_back(out_path.substr(i, e - i));
    i = e + 1;
  }
  normalize_path(segs);
  std::string norm;
  for (auto& s : segs) norm += "/" + s;
  if (trailing_slash && !norm.empty()) norm += "/";
  if (norm.empty()) norm = "/";
  if (bs == "file") {
    std::string out = "file://" + (bh.empty() ? "" : bh) + norm + out_query;
    return out;
  }
  std::string out = bs + "://" + bh;
  if ((bs == "http" && bp != "80") || (bs == "https" && bp != "443")) out += ":" + bp;
  out += norm + out_query;
  return out;
}

/* ============================ codecs ============================ */
static int b64val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}
std::string base64_decode(const std::string& in) {
  std::string out;
  int val = 0, bits = 0;
  for (char c : in) {
    if (isspace((unsigned char)c) || c == '=') continue;
    int v = b64val(c);
    if (v < 0) continue;
    val = (val << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back((char)((val >> bits) & 0xFF));
    }
  }
  return out;
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
static std::string percent_decode(const std::string& s, bool plus_space) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size()) {
      int h = hexval(s[i + 1]), l = hexval(s[i + 2]);
      if (h >= 0 && l >= 0) { out.push_back((char)((h << 4) | l)); i += 2; continue; }
    }
    if (plus_space && s[i] == '+') { out.push_back(' '); continue; }
    out.push_back(s[i]);
  }
  return out;
}

bool data_url_parse(const std::string& url, std::string* mime, std::vector<uint8_t>* bytes) {
  if (url.rfind("data:", 0) != 0) return false;
  size_t comma = url.find(',');
  if (comma == std::string::npos) return false;
  std::string head = url.substr(5, comma - 5);
  std::string payload = url.substr(comma + 1);
  bool is_b64 = false;
  std::string m = "text/plain";
  if (!head.empty()) {
    auto parts = head;
    std::string lower = lower_copy(parts);
    size_t b64 = lower.find(";base64");
    if (b64 != std::string::npos) { is_b64 = true; parts = parts.substr(0, b64); }
    if (!parts.empty()) m = parts;
  }
  std::string decoded = is_b64 ? base64_decode(payload) : percent_decode(payload, false);
  if (mime) *mime = m;
  if (bytes) bytes->assign(decoded.begin(), decoded.end());
  return true;
}

bool gzip_inflate(const uint8_t* in, size_t n, std::vector<uint8_t>* out) {
  if (!in || n == 0 || !out) return false;
  out->clear();
  out->reserve(n * 4);
  for (int attempt = 0; attempt < 2; attempt++) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    /* attempt 0: auto gzip/zlib (32+15), attempt 1: raw deflate (-15) */
    if (inflateInit2(&zs, attempt == 0 ? 32 + 15 : -15) != Z_OK) return false;
    zs.next_in = const_cast<Bytef*>(in);
    zs.avail_in = (uInt)n;
    uint8_t buf[65536];
    int rc = Z_OK;
    bool fail = false;
    do {
      zs.next_out = buf;
      zs.avail_out = sizeof(buf);
      rc = inflate(&zs, Z_NO_FLUSH);
      if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) { fail = true; break; }
      out->insert(out->end(), buf, buf + (sizeof(buf) - zs.avail_out));
      if (out->size() > 512u * 1024 * 1024) { fail = true; break; }
    } while (rc != Z_STREAM_END && zs.avail_in > 0);
    inflateEnd(&zs);
    if (!fail && rc == Z_STREAM_END) return true;
    out->clear();
  }
  return false;
}

/* ============================ cookies ============================ */
void CookieJar::store(const std::string& host, const std::string& set_cookie_value) {
  std::string pair = set_cookie_value;
  size_t semi = pair.find(';');
  if (semi != std::string::npos) pair = pair.substr(0, semi);
  size_t eq = pair.find('=');
  if (eq == std::string::npos) return;
  std::string name = trim_copy(pair.substr(0, eq));
  std::string value = trim_copy(pair.substr(eq + 1));
  if (name.empty()) return;
  std::lock_guard<std::mutex> lk(mtx);
  for (auto& c : cookies)
    if (c.host == host && c.name == name) { c.value = value; return; }
  cookies.push_back(Cookie{host, name, value});
}
std::string CookieJar::get(const std::string& host) const {
  std::string out;
  std::lock_guard<std::mutex> lk(mtx);
  for (auto& c : cookies) {
    if (c.host == host || (host.size() > c.host.size() + 1 &&
        host.compare(host.size() - c.host.size() - 1, c.host.size() + 1, "." + c.host) == 0)) {
      if (!out.empty()) out += "; ";
      out += c.name + "=" + c.value;
    }
  }
  return out;
}

/* ============================ sockets ============================ */
struct TcpSocket {
  int fd = -1;
  ~TcpSocket() { close_fd(); }
  void close_fd() {
#ifdef _WIN32
    if (fd >= 0) { closesocket(fd); fd = -1; }
#else
    if (fd >= 0) { ::close(fd); fd = -1; }
#endif
  }
  bool valid() const { return fd >= 0; }
};

static int tcp_connect_timeout(const std::string& host, const std::string& port, int timeout_ms, std::string* err) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = nullptr;
#ifdef _WIN32
  int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
#else
  int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
#endif
  if (rc != 0 || !res) { if (err) *err = "DNS lookup failed for " + host; return -1; }
  int fd = -1;
  std::string last_err = "connect failed";
  for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    /* nonblocking connect with poll timeout */
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(fd, FIONBIO, &nb);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
    rc = connect(fd, ai->ai_addr, (int)ai->ai_addrlen);
    if (rc != 0) {
#ifdef _WIN32
      fd_set wset;
      FD_ZERO(&wset);
      FD_SET((SOCKET)fd, &wset);
      struct timeval tv;
      tv.tv_sec = timeout_ms / 1000;
      tv.tv_usec = (timeout_ms % 1000) * 1000;
      int pr = select(0, nullptr, &wset, nullptr, &tv);
#else
      struct pollfd pfd;
      pfd.fd = fd;
      pfd.events = POLLOUT;
      int pr = poll(&pfd, 1, timeout_ms);
#endif
      if (pr <= 0) {
        last_err = pr == 0 ? "connect timeout" : last_err;
        TcpSocket tmp; tmp.fd = fd; tmp.close_fd(); fd = -1;
        continue;
      }
      int soerr = 0;
      socklen_t slen = sizeof(soerr);
      getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&soerr, &slen);
      if (soerr != 0) {
        TcpSocket tmp; tmp.fd = fd; tmp.close_fd(); fd = -1;
        continue;
      }
    }
#ifdef _WIN32
    u_long nb0 = 0;
    ioctlsocket(fd, FIONBIO, &nb0);
#else
    int fl2 = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl2 & ~O_NONBLOCK);
#endif
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
#ifdef _WIN32
    /* NOTE: do NOT set SO_RCVTIMEO on Windows — on some systems (LSP/AV
     * interference) timed recv() fails instantly with WSAETIMEDOUT. Read
     * timeouts are implemented with select() before each recv instead. */
    (void)tv;
#else
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#endif
    break;
  }
  freeaddrinfo(res);
  if (fd < 0 && err) *err = last_err + " (" + host + ":" + port + ")";
  return fd;
}

static bool send_all(int fd, const char* data, size_t n) {
  size_t off = 0;
  while (off < n) {
#ifdef _WIN32
    int w = send(fd, data + off, (int)(n - off), 0);
#else
    ssize_t w = ::send(fd, data + off, n - off, MSG_NOSIGNAL);
#endif
    if (w <= 0) return false;
    off += (size_t)w;
  }
  return true;
}

/* ============================ HTTP/1.1 over a stream ============================ */
struct StreamOps {
  /* return: >0 bytes read, 0 = eof, <0 error */
  std::function<int(char*, size_t)> read;
  std::function<bool(const char*, size_t)> write;
};

static bool read_http_response(StreamOps& io, HttpResult* res, const std::atomic<bool>* cancel) {
  std::string buf;
  size_t hdr_end;
  for (;;) {
    if (cancel && cancel->load()) { res->err = "cancelled"; return false; }
    hdr_end = buf.find("\r\n\r\n");
    if (hdr_end != std::string::npos) break;
    if (buf.size() > 256 * 1024) { res->err = "response headers too large"; return false; }
    char tmp[32768];
    int r = io.read(tmp, sizeof(tmp));
    if (r < 0) { res->err = "connection error while reading response headers"; return false; }
    if (r == 0) { res->err = "connection closed before response headers complete"; return false; }
    buf.append(tmp, (size_t)r);
  }
  /* status line */
  size_t line_end = buf.find("\r\n");
  std::string status_line = buf.substr(0, line_end);
  if (status_line.rfind("HTTP/", 0) != 0) { res->err = "bad status line: " + status_line.substr(0, 40); return false; }
  size_t sp1 = status_line.find(' ');
  res->status = sp1 == std::string::npos ? 0 : atoi(status_line.c_str() + sp1 + 1);
  /* headers */
  size_t pos = line_end + 2;
  while (pos < hdr_end) {
    size_t e = buf.find("\r\n", pos);
    if (e == std::string::npos || e > hdr_end) e = hdr_end;
    std::string line = buf.substr(pos, e - pos);
    pos = e + 2;
    size_t c = line.find(':');
    if (c == std::string::npos) continue;
    res->headers.push_back(HttpHeader{trim_copy(line.substr(0, c)), trim_copy(line.substr(c + 1))});
  }
  std::string body = buf.substr(hdr_end + 4);
  /* body decoding */
  std::string te = res->header("Transfer-Encoding");
  std::string cl = res->header("Content-Length");
  if (!te.empty() && lower_copy(te).find("chunked") != std::string::npos) {
    for (;;) {
      if (cancel && cancel->load()) { res->err = "cancelled"; return false; }
      size_t eol = body.find("\r\n");
      while (eol == std::string::npos) {
        if (body.size() > 256ull * 1024 * 1024) { res->err = "chunked body too large"; return false; }
        char tmp[32768];
        int r = io.read(tmp, sizeof(tmp));
        if (r <= 0) break;
        body.append(tmp, (size_t)r);
        eol = body.find("\r\n");
      }
      if (eol == std::string::npos) break;
      std::string size_str = body.substr(0, eol);
      size_t semi = size_str.find(';');
      if (semi != std::string::npos) size_str = size_str.substr(0, semi);
      unsigned long long chunk = strtoull(size_str.c_str(), nullptr, 16);
      body.erase(0, eol + 2);
      if (chunk == 0) {
        /* trailers until empty line */
        size_t t = body.find("\r\n");
        while (t == std::string::npos && body.size() < 65536) {
          char tmp[4096];
          int r = io.read(tmp, sizeof(tmp));
          if (r <= 0) break;
          body.append(tmp, (size_t)r);
          t = body.find("\r\n");
        }
        body.clear();
        break;
      }
      while (body.size() < chunk + 2) {
        char tmp[32768];
        int r = io.read(tmp, sizeof(tmp));
        if (r <= 0) break;
        body.append(tmp, (size_t)r);
      }
      if (body.size() < chunk + 2) break;
      res->body.insert(res->body.end(), body.begin(), body.begin() + chunk);
      body.erase(0, chunk + 2);
      if (res->body.size() > 256ull * 1024 * 1024) { res->err = "body too large"; return false; }
    }
  } else if (!cl.empty()) {
    unsigned long long need = strtoull(cl.c_str(), nullptr, 10);
    while (body.size() < need) {
      if (cancel && cancel->load()) { res->err = "cancelled"; return false; }
      char tmp[32768];
      int r = io.read(tmp, sizeof(tmp));
      if (r <= 0) break;
      body.append(tmp, (size_t)r);
    }
    if (body.size() > need) body.resize((size_t)need);
    res->body.assign(body.begin(), body.end());
  } else {
    /* read to EOF */
    for (;;) {
      if (cancel && cancel->load()) break;
      char tmp[32768];
      int r = io.read(tmp, sizeof(tmp));
      if (r <= 0) break;
      body.append(tmp, (size_t)r);
      if (body.size() > 256ull * 1024 * 1024) break;
    }
    res->body.assign(body.begin(), body.end());
  }
  std::string enc = lower_copy(res->header("Content-Encoding"));
  if (enc.find("gzip") != std::string::npos || enc.find("deflate") != std::string::npos) {
    std::vector<uint8_t> inflated;
    if (gzip_inflate(res->body.data(), res->body.size(), &inflated)) res->body = std::move(inflated);
    /* if inflate fails, keep raw (server lied) */
  }
  return true;
}

/* ============================ TLS: OpenSSL ============================ */
#ifndef _WIN32
struct TlsSession {
  SSL_CTX* ctx = nullptr;
  SSL* ssl = nullptr;
  int fd = -1;
  ~TlsSession() {
    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
    if (ctx) SSL_CTX_free(ctx);
  }
  bool read_pending() { return ssl && SSL_pending(ssl) > 0; }
};

static int tls_read(char* out, size_t n, void* ud) {
  TlsSession* t = (TlsSession*)ud;
  int r = SSL_read(t->ssl, out, (int)n);
  if (r > 0) return r;
  int err = SSL_get_error(t->ssl, r);
  if (err == SSL_ERROR_ZERO_RETURN) return 0;
  if (err == SSL_ERROR_SYSCALL && (r == 0)) return 0;
  return -1;
}
static bool tls_write(const char* d, size_t n, void* ud) {
  TlsSession* t = (TlsSession*)ud;
  size_t off = 0;
  while (off < n) {
    int w = SSL_write(t->ssl, d + off, (int)(n - off));
    if (w <= 0) return false;
    off += (size_t)w;
  }
  return true;
}

static bool https_session(TcpSocket& sock, const std::string& host, int timeout_ms, TlsSession** out, std::string* err, bool* h2_negotiated) {
  static bool no_h2 = getenv("SQRL_NO_H2") != nullptr;
  if (h2_negotiated) *h2_negotiated = false;
  if (no_h2) h2_negotiated = nullptr;
  (void)timeout_ms;
  TlsSession* t = new TlsSession();
  t->fd = sock.fd;
  t->ctx = SSL_CTX_new(TLS_client_method());
  if (!t->ctx) { *err = "SSL_CTX_new failed"; delete t; return false; }
  SSL_CTX_set_verify(t->ctx, SSL_VERIFY_PEER, nullptr);
  if (!SSL_CTX_set_default_verify_paths(t->ctx)) { *err = "no CA bundle found (install ca-certificates)"; delete t; return false; }
  SSL_CTX_set_min_proto_version(t->ctx, TLS1_2_VERSION);
  /* ALPN: prefer HTTP/2, fall back to HTTP/1.1 (RFC 7301) */
  static const unsigned char alpn[] = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
  SSL_CTX_set_alpn_protos(t->ctx, alpn, sizeof(alpn));
  t->ssl = SSL_new(t->ctx);
  if (!t->ssl) { *err = "SSL_new failed"; delete t; return false; }
  SSL_set_fd(t->ssl, sock.fd);
  SSL_set_tlsext_host_name(t->ssl, host.c_str());
  SSL_set1_host(t->ssl, host.c_str());
  SSL_set_mode(t->ssl, SSL_MODE_AUTO_RETRY);
  for (;;) {
    int rc = SSL_connect(t->ssl);
    if (rc == 1) break;
    int e = SSL_get_error(t->ssl, rc);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
      struct pollfd pfd;
      pfd.fd = sock.fd;
      pfd.events = e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
      if (poll(&pfd, 1, timeout_ms) <= 0) { *err = "TLS handshake timeout"; delete t; return false; }
      continue;
    }
    unsigned long ec = ERR_get_error();
    char eb[256];
    ERR_error_string_n(ec, eb, sizeof(eb));
    *err = std::string("TLS handshake failed: ") + eb;
    delete t;
    return false;
  }
  long vr = SSL_get_verify_result(t->ssl);
  if (vr != X509_V_OK) { *err = std::string("certificate verify failed: ") + X509_verify_cert_error_string(vr); delete t; return false; }
  if (h2_negotiated) {
    const unsigned char* alpn_sel = nullptr;
    unsigned int alpn_len = 0;
    SSL_get0_alpn_selected(t->ssl, &alpn_sel, &alpn_len);
    *h2_negotiated = alpn_sel && alpn_len == 2 && memcmp(alpn_sel, "h2", 2) == 0;
  }
  *out = t;
  return true;
}
#endif /* !_WIN32 */

/* ============================ Schannel (Windows) ============================ */
#ifdef _WIN32
/* A compact TLS-over-socket stream using Schannel with in-memory buffering. */
struct SchannelStream {
  SOCKET sock = INVALID_SOCKET;
  CredHandle cred{};
  CtxtHandle ctx{};
  bool ctx_init = false;
  SecPkgContext_StreamSizes sizes{};
  std::string last_err;   /* diagnostics for the last failed operation */
  std::string plain;      /* decrypted plaintext buffer */
  std::string cipher;     /* undecrypted ciphertext buffer */
  bool eof = false;

  ~SchannelStream() {
    if (ctx_init) DeleteSecurityContext(&ctx);
    FreeCredentialsHandle(&cred);
  }

  bool init(const std::string& host, std::string* err) {
    SCHANNEL_CRED sc;
    memset(&sc, 0, sizeof(sc));
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    /* TLS 1.2 only: the legacy SCHANNEL_CRED path mishandles TLS 1.3 with
     * older SDK headers, and some servers reject the resulting ClientHello. */
    sc.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
    sc.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS |
                 SCH_USE_STRONG_CRYPTO;
    TimeStamp exp;
    SECURITY_STATUS st = AcquireCredentialsHandleA(nullptr, (SEC_CHAR*)UNISP_NAME_A,
                                                   SECPKG_CRED_OUTBOUND, nullptr, &sc, nullptr,
                                                   nullptr, &cred, &exp);
    if (st != SEC_E_OK) { *err = "AcquireCredentialsHandle failed"; return false; }

    ULONG attrs = 0;
    std::string hostn = host;
    /* first InitializeSecurityContext call: NO input token (we start the
     * handshake). Passing the hostname as a SECBUFFER_TOKEN would send those
     * raw bytes to the server instead of a ClientHello → handshake failure. */
    SecBuffer out0[1];
    out0[0].BufferType = SECBUFFER_TOKEN;
    out0[0].cbBuffer = 0;
    out0[0].pvBuffer = nullptr;
    SecBufferDesc out0_desc = { SECBUFFER_VERSION, 1, out0 };
    st = InitializeSecurityContextA(
        &cred, nullptr, (SEC_CHAR*)hostn.c_str(),
        ISC_REQ_STREAM | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_CONFIDENTIALITY |
            ISC_REQ_REPLAY_DETECT | ISC_REQ_SEQUENCE_DETECT | ISC_REQ_EXTENDED_ERROR |
            ISC_REQ_USE_SUPPLIED_CREDS,
        0, SECURITY_NATIVE_DREP, nullptr, 0, &ctx, &out0_desc, &attrs, &exp);
    if (st != SEC_I_CONTINUE_NEEDED && st != SEC_E_OK && st != SEC_I_COMPLETE_NEEDED &&
        st != SEC_I_COMPLETE_AND_CONTINUE) {
      *err = "ISC init failed";
      return false;
    }
    ctx_init = true;
    /* send initial ClientHello token if any */
    if (out0[0].cbBuffer && out0[0].pvBuffer) {
      bool ok = sock_send((const char*)out0[0].pvBuffer, out0[0].cbBuffer);
      FreeContextBuffer(out0[0].pvBuffer);
      if (!ok) { *err = "send failed (initial token)"; return false; }
    }
    /* handshake loop */
    for (int iter = 0; iter < 20; iter++) {
      if (st == SEC_E_OK) break;
      if (cipher.empty()) {
        if (!sock_recv_more(err)) return false;
      }
      SecBuffer in[2] = {};   /* zeroed: SSPI marshals every element */
      in[0].BufferType = SECBUFFER_TOKEN;
      in[0].cbBuffer = (ULONG)cipher.size();
      in[0].pvBuffer = cipher.empty() ? nullptr : (void*)cipher.data();
      in[1].BufferType = SECBUFFER_EMPTY;
      SecBufferDesc in_desc = { SECBUFFER_VERSION, 2, in };
      SecBuffer out[3] = {};
      SecBufferDesc out_desc = { SECBUFFER_VERSION, 3, out };
      ULONG a2 = 0;
      st = InitializeSecurityContextA(&cred, &ctx, (SEC_CHAR*)hostn.c_str(),
                                      ISC_REQ_STREAM | ISC_REQ_ALLOCATE_MEMORY |
                                      ISC_REQ_CONFIDENTIALITY | ISC_REQ_REPLAY_DETECT |
                                      ISC_REQ_SEQUENCE_DETECT | ISC_REQ_EXTENDED_ERROR,
                                      0, SECURITY_NATIVE_DREP, &in_desc, 0, nullptr,
                                      &out_desc, &a2, &exp);
      if (st == SEC_E_INCOMPLETE_MESSAGE) {
        if (getenv("SQRL_TLS_DEBUG")) fprintf(stderr, "[tls] isc incomplete, recv\n");
        if (!sock_recv_more(err)) return false;
        continue;
      }
      static bool tls_dbg2 = getenv("SQRL_TLS_DEBUG") != nullptr;
      if (tls_dbg2) fprintf(stderr, "[tls] isc iter=%d st=%08lx cin=%zu\n", iter, (unsigned long)st, cipher.size());
      if (FAILED(st) && st != SEC_I_CONTINUE_NEEDED && st != SEC_E_OK &&
          st != SEC_I_COMPLETE_NEEDED && st != SEC_I_COMPLETE_AND_CONTINUE) {
        *err = "handshake failed";
        return false;
      }
      /* consumed bytes: EXTRA is the number of UNPROCESSED bytes at the END
       * of the input — keep those, drop everything before them */
      if (in[1].BufferType == SECBUFFER_EXTRA && in[1].cbBuffer)
        cipher.erase(0, cipher.size() - in[1].cbBuffer);
      else
        cipher.clear();
      for (ULONG i = 0; i < out_desc.cBuffers; i++)
        if (out[i].cbBuffer && out[i].pvBuffer && out[i].BufferType == SECBUFFER_TOKEN) {
          bool ok = sock_send((const char*)out[i].pvBuffer, out[i].cbBuffer);
          FreeContextBuffer(out[i].pvBuffer);
          if (!ok) { *err = "send failed (loop token)"; return false; }
        }
      if (st == SEC_I_COMPLETE_NEEDED || st == SEC_I_COMPLETE_AND_CONTINUE)
        CompleteAuthToken(&ctx, &out_desc);
      if (st == SEC_I_CONTINUE_NEEDED || st == SEC_I_COMPLETE_AND_CONTINUE) {
        if (!sock_recv_more(err)) return false;
      }
    }
    if (st != SEC_E_OK) { *err = "handshake did not complete"; return false; }
    SECURITY_STATUS qs = QueryContextAttributesA(&ctx, SECPKG_ATTR_STREAM_SIZES, &sizes);
    if (qs != SEC_E_OK) { *err = "stream sizes query failed"; return false; }
    if (getenv("SQRL_TLS_DEBUG")) fprintf(stderr, "[tls] handshake ok\n");
    return true;
  }

  bool sock_send(const char* d, size_t n) {
    size_t off = 0;
    while (off < n) {
      int w = send(sock, d + off, (int)(n - off), 0);
      if (w <= 0) {
        if (getenv("SQRL_TLS_DEBUG"))
          fprintf(stderr, "[tls] send w=%d wsa=%d (at %zu/%zu)\n", w, (int)WSAGetLastError(), off, n);
        return false;
      }
      off += (size_t)w;
    }
    return true;
  }
  bool sock_recv_more(std::string* err) {
#ifdef _WIN32
    /* select-based read timeout (SO_RCVTIMEO unreliable, see tcp_connect_timeout) */
    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(sock, &rset);
    struct timeval tv;
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    int pr = select((int)sock + 1, &rset, nullptr, nullptr, &tv);
    if (pr <= 0) {
      eof = true;
      if (err) *err = "read timeout / socket not ready";
      return false;
    }
#endif
    char tmp[16384];
    int r = recv(sock, tmp, sizeof(tmp), 0);
    if (r <= 0) {
      eof = true;
      if (getenv("SQRL_TLS_DEBUG"))
        fprintf(stderr, "[tls] recv r=%d wsa=%d\n", r, (int)WSAGetLastError());
      if (err) *err = "connection closed during handshake/read";
      return false;
    }
    cipher.append(tmp, (size_t)r);
    return true;
  }

  int decrypt_available() {
    for (int guard = 0; guard < 64; guard++) {
      if (cipher.size() < sizes.cbHeader) return 0;
      SecBuffer bufs[4] = {}; /* zeroed: DecryptMessage writes EXTRA/EMPTY info here */
      bufs[0].BufferType = SECBUFFER_DATA; bufs[0].cbBuffer = (ULONG)cipher.size(); bufs[0].pvBuffer = (void*)cipher.data();
      bufs[1].BufferType = SECBUFFER_EMPTY; bufs[2].BufferType = SECBUFFER_EMPTY; bufs[3].BufferType = SECBUFFER_EMPTY;
      SecBufferDesc d = { SECBUFFER_VERSION, 4, bufs };
      SECURITY_STATUS st = DecryptMessage(&ctx, &d, 0, nullptr);
      static bool tls_dbg = getenv("SQRL_TLS_DEBUG") != nullptr;
      if (tls_dbg) {
        fprintf(stderr, "[tls] decrypt st=%08lx cin=%zu out=%zu |", (unsigned long)st,
                cipher.size(), plain.size());
        for (int bi = 0; bi < 4; bi++)
          fprintf(stderr, " [%d]=t%u n%u", bi, (unsigned)bufs[bi].BufferType,
                  (unsigned)bufs[bi].cbBuffer);
        fprintf(stderr, "\n");
      }
      if (st == SEC_E_INCOMPLETE_MESSAGE) return 0;
      if (FAILED(st)) return -1;
      /* DecryptMessage reorders the array: scan ALL buffers for DATA/EXTRA */
      size_t cin_before = cipher.size();
      ULONG extra_n = 0;
      for (int bi = 0; bi < 4; bi++) {
        if (bufs[bi].BufferType == SECBUFFER_DATA && bufs[bi].cbBuffer) {
          plain.append((const char*)bufs[bi].pvBuffer, bufs[bi].cbBuffer);
        } else if (bufs[bi].BufferType == SECBUFFER_EXTRA && bufs[bi].cbBuffer) {
          extra_n = bufs[bi].cbBuffer;
        }
      }
      if (tls_dbg && !plain.empty()) {
        std::string head = plain.substr(0, plain.size() < 80 ? plain.size() : 80);
        fprintf(stderr, "[tls] plain head: %.80s\n", head.c_str());
      }
      /* EXTRA = unprocessed bytes at the END; keep those, drop the rest */
      if (extra_n) cipher.erase(0, cin_before - extra_n);
      else cipher.clear();
      if (st == SEC_I_CONTEXT_EXPIRED) { eof = true; return (int)plain.size(); }
      if (cipher.empty()) return (int)plain.size();
    }
    return (int)plain.size();
  }

  int read(char* out, size_t n) {
    while (plain.empty()) {
      if (eof) return 0;
      if (!sock_recv_more(nullptr)) { if (plain.empty()) return 0; break; }
      int rc = decrypt_available();
      if (rc < 0) return -1;
    }
    size_t take = n < plain.size() ? n : plain.size();
    memcpy(out, plain.data(), take);
    plain.erase(0, take);
    return (int)take;
  }

  bool write(const char* d, size_t n) {
    while (n > 0) {
      size_t chunk = n < sizes.cbMaximumMessage ? n : sizes.cbMaximumMessage;
      std::vector<char> buf(sizes.cbHeader + chunk + sizes.cbTrailer);
      SecBuffer bufs[3];
      bufs[0].BufferType = SECBUFFER_STREAM_HEADER; bufs[0].cbBuffer = sizes.cbHeader; bufs[0].pvBuffer = buf.data();
      bufs[1].BufferType = SECBUFFER_DATA; bufs[1].cbBuffer = (ULONG)chunk; bufs[1].pvBuffer = buf.data() + sizes.cbHeader;
      bufs[2].BufferType = SECBUFFER_STREAM_TRAILER; bufs[2].cbBuffer = sizes.cbTrailer; bufs[2].pvBuffer = buf.data() + sizes.cbHeader + chunk;
      memcpy(bufs[1].pvBuffer, d, chunk);
      SecBufferDesc desc = { SECBUFFER_VERSION, 3, bufs };
      SECURITY_STATUS ess = EncryptMessage(&ctx, 0, &desc, 0);
      if (FAILED(ess)) {
        char eb[64];
        snprintf(eb, sizeof(eb), "encrypt failed %08lx (hdr=%lu max=%lu trl=%lu)", (unsigned long)ess,
                 (unsigned long)sizes.cbHeader, (unsigned long)sizes.cbMaximumMessage,
                 (unsigned long)sizes.cbTrailer);
        last_err = eb;
        return false;
      }
      if (!sock_send(buf.data(), bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer)) {
        char eb[64];
        snprintf(eb, sizeof(eb), "tls send failed wsa=%d", (int)WSAGetLastError());
        last_err = eb;
        return false;
      }
      d += chunk; n -= chunk;
    }
    return true;
  }
};
#endif /* _WIN32 */

/* ============================ HTTP/2 (RFC 7540/7541) ============================ */
static const char* kDefaultUA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/131.0.0.0 Safari/537.36 SquareEngine/1.0";

#include "square_h2.h"

#ifndef _WIN32
/* run one GET/HEAD over an ALPN-negotiated h2 connection.
 * Returns true when the response was produced (ok or a clean HTTP error). */
static bool h2_perform(TlsSession* tls, const std::string& method, const std::string& host,
                       const std::string& pq, const std::string& scheme,
                       const std::vector<HttpHeader>& extra_headers, CookieJar* jar,
                       const std::atomic<bool>* cancel, int timeout_ms, HttpResult* res,
                       std::string* err) {
  StreamOps io;
  io.read = [tls](char* d, size_t n) { return tls_read(d, n, tls); };
  io.write = [tls](const char* d, size_t n) { return tls_write(d, n, tls); };

  /* connection preface + empty SETTINGS + big connection window */
  std::string preface = h2_preface();
  if (!io.write(preface.data(), preface.size())) { *err = "h2: preface send failed"; return false; }
  {
    /* empty SETTINGS (defaults), stream 0 */
    uint8_t f[9] = {0, 0, 0, H2_SETTINGS, 0, 0, 0, 0, 0};
    if (!io.write((const char*)f, 9)) { *err = "h2: settings send failed"; return false; }
  }
  {
    /* WINDOW_UPDATE (stream 0): 9-byte header + 4-byte payload */
    uint8_t wu[13] = {0, 0, 4, H2_WINDOW_UPDATE, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint32_t inc = 1u << 24;
    wu[9] = (uint8_t)(inc >> 24); wu[10] = (uint8_t)(inc >> 16); wu[11] = (uint8_t)(inc >> 8); wu[12] = (uint8_t)inc;
    if (!io.write((const char*)wu, 13)) { *err = "h2: window update failed"; return false; }
  }

  /* HEADERS on stream 1 */
  std::vector<std::pair<std::string, std::string>> hdrs;
  hdrs.push_back({":method", method});
  hdrs.push_back({":scheme", scheme});
  hdrs.push_back({":path", pq.empty() ? "/" : pq});
  hdrs.push_back({":authority", host});
  std::string cookie_str = jar ? jar->get(host) : "";
  bool have_ua = false, have_accept = false, have_ae = false;
  for (auto& h : extra_headers) {
    std::string ln = lower_copy(h.name);
    if (ln == "cookie") { cookie_str = cookie_str.empty() ? h.value : cookie_str + "; " + h.value; continue; }
    if (ln == "host" || ln == "connection" || ln == "transfer-encoding" || ln == "keep-alive" ||
        ln == "upgrade-insecure-requests" || ln == "proxy-connection")
      continue; /* forbidden / managed in h2 */
    if (ln == "user-agent") have_ua = true;
    if (ln == "accept") have_accept = true;
    if (ln == "accept-encoding") have_ae = true;
    hdrs.push_back({lower_copy(h.name), h.value});
  }
  if (!have_ua) hdrs.push_back({"user-agent", kDefaultUA});
  if (!have_accept)
    hdrs.push_back({"accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8"});
  if (!have_ae) hdrs.push_back({"accept-encoding", "gzip, deflate"});
  if (!cookie_str.empty()) hdrs.push_back({"cookie", cookie_str});

  std::vector<uint8_t> block;
  hpack_encode_block(hdrs, &block);
  /* frame: len(3) type(1) flags(1) stream(4) */
  std::vector<uint8_t> frame(9 + block.size());
  frame[0] = (uint8_t)(block.size() >> 16);
  frame[1] = (uint8_t)(block.size() >> 8);
  frame[2] = (uint8_t)block.size();
  frame[3] = H2_HEADERS;
  /* GET/HEAD have no body: the request HEADERS frame must END the stream */
  frame[4] = F_END_HEADERS | (method == "GET" || method == "HEAD" ? F_END_STREAM : 0);
  frame[5] = 0; frame[6] = 0; frame[7] = 0; frame[8] = 1; /* stream 1 */
  memcpy(frame.data() + 9, block.data(), block.size());
  if (getenv("SQRL_H2_DEBUG")) {
    fprintf(stderr, "[h2] TX HEADERS frame len=%zu zb: %02x %02x %02x | type=%02x flags=%02x | sid=%02x%02x%02x%02x\n",
            frame.size(), frame[0], frame[1], frame[2], frame[3], frame[4], frame[5], frame[6],
            frame[7], frame[8]);
    fprintf(stderr, "[h2] TX block first 24: ");
    for (size_t i = 0; i < 24 && i < block.size(); i++) fprintf(stderr, "%02x ", block[i]);
    fprintf(stderr, "\n");
  }
  if (!io.write((const char*)frame.data(), frame.size())) { *err = "h2: headers send failed"; return false; }

  /* read frames until END_STREAM on stream 1 */
  std::vector<uint8_t> body;
  int status = 0;
  bool saw_headers = false;
  std::vector<std::pair<std::string, std::string>> resp_headers;
  HpackDynTable dyn;
  auto start = std::chrono::steady_clock::now();
  auto timed_out = [&]() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start).count() > timeout_ms;
  };
  std::string pending_block; /* CONTINUATION assembly */
  bool in_headers = false;
  while (true) {
    if (cancel && cancel->load()) { *err = "cancelled"; return false; }
    if (timed_out()) { *err = "h2: timeout"; return false; }
    uint8_t fh[9];
    int got = 0;
    while (got < 9) {
      int r = io.read((char*)fh + got, 9 - got);
      if (r <= 0) { if (!got) { *err = "h2: connection closed"; return false; } return false; }
      got += r;
    }
    size_t flen = ((size_t)fh[0] << 16) | ((size_t)fh[1] << 8) | fh[2];
    uint8_t ftype = fh[3], fflags = fh[4];
    uint32_t sid = ((uint32_t)fh[5] << 24) | ((uint32_t)fh[6] << 16) | ((uint32_t)fh[7] << 8) | fh[8];
    sid &= 0x7fffffffu;
    if (getenv("SQRL_H2_DEBUG"))
      fprintf(stderr, "[h2] frame type=%d flags=%d len=%zu sid=%u\n", ftype, fflags, flen, sid);
    if (flen > (64u << 20)) { *err = "h2: frame too large"; return false; }
    std::vector<uint8_t> payload(flen);
    size_t have = 0;
    while (have < flen) {
      int r = io.read((char*)payload.data() + have, flen - have);
      if (r <= 0) { *err = "h2: stream closed mid-frame"; return false; }
      have += (size_t)r;
    }
    switch (ftype) {
      case H2_SETTINGS: {
        if (!(fflags & FACK)) {
          /* acknowledge: empty SETTINGS with ACK */
          uint8_t ack[9] = {0, 0, 0, H2_SETTINGS, FACK, 0, 0, 0, 0};
          if (!io.write((const char*)ack, 9)) { *err = "h2: ack failed"; return false; }
        }
        break;
      }
      case H2_PING: {
        if (!(fflags & FACK)) {
          uint8_t pkt[17] = {0, 0, 8, H2_PING, FACK, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
          for (int i = 0; i < 8 && (size_t)i < flen; i++) pkt[9 + i] = payload[i];
          if (!io.write((const char*)pkt, 17)) { *err = "h2: ping ack failed"; return false; }
        }
        break;
      }
      case H2_WINDOW_UPDATE:
      case H2_PRIORITY:
        break; /* nothing to do; we keep windows open proactively */
      case H2_PUSH_PROMISE: {
        /* refuse server push: RST_STREAM(REFUSED_STREAM) on the *promised*
         * stream (payload first 4 bytes), never on our own request stream */
        if (flen >= 4) {
          uint32_t psid = ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) |
                          ((uint32_t)payload[2] << 8) | payload[3];
          psid &= 0x7fffffffu;
          uint8_t rst[13] = {0, 0, 4, H2_RST_STREAM, 0, 0, 0, 0, 0, 0, 0, 0, 7};
          rst[8] = (uint8_t)(psid >> 24); rst[9] = (uint8_t)(psid >> 16);
          rst[10] = (uint8_t)(psid >> 8); rst[11] = (uint8_t)psid;
          if (!io.write((const char*)rst, 13)) { *err = "h2: rst failed"; return false; }
        }
        break;
      }
      case H2_RST_STREAM:
        if (sid == 1) {
          if (getenv("SQRL_H2_DEBUG") && flen >= 4) {
            uint32_t ec = ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) |
                          ((uint32_t)payload[2] << 8) | payload[3];
            fprintf(stderr, "[h2] RST_STREAM error=%u\n", ec);
          }
          *err = "h2: stream reset by server";
          return false;
        }
        break;
      case H2_GOAWAY: {
        if (getenv("SQRL_H2_DEBUG") && flen >= 8) {
          uint32_t errc = ((uint32_t)payload[4] << 24) | ((uint32_t)payload[5] << 16) |
                          ((uint32_t)payload[6] << 8) | payload[7];
          fprintf(stderr, "[h2] GOAWAY error=%u last_sid=%u debug='%.40s'\n", errc,
                  ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) |
                      ((uint32_t)payload[2] << 8) | payload[3],
                  flen > 8 ? (const char*)payload.data() + 8 : "");
        }
        if (body.empty() && !saw_headers) { *err = "h2: GOAWAY before response"; return false; }
        break;
      }
      case H2_HEADERS: {
        size_t off = 0;
        if (fflags & F_PADDED) off += 1;
        if (fflags & F_PRIORITY) off += 5;
        if (off > flen) { *err = "h2: bad headers frame"; return false; }
        pending_block.assign((const char*)payload.data() + off, flen - off);
        in_headers = true;
        if (fflags & F_END_HEADERS) {
          if (!hpack_decode_block((const uint8_t*)pending_block.data(), pending_block.size(), dyn,
                                  &resp_headers))
            { *err = "h2: hpack decode failed"; return false; }
          in_headers = false;
          saw_headers = true;
          for (auto& kv : resp_headers)
            if (kv.first == ":status") status = atoi(kv.second.c_str());
        }
        break;
      }
      case H2_CONTINUATION: {
        pending_block.append((const char*)payload.data(), payload.size());
        if (fflags & F_END_HEADERS) {
          if (!hpack_decode_block((const uint8_t*)pending_block.data(), pending_block.size(), dyn,
                                  &resp_headers))
            { *err = "h2: hpack decode failed"; return false; }
          in_headers = false;
          saw_headers = true;
          for (auto& kv : resp_headers)
            if (kv.first == ":status") status = atoi(kv.second.c_str());
        }
        break;
      }
      case H2_DATA: {
        size_t off = 0;
        if (fflags & F_PADDED) off += 1;
        if (off <= flen) body.insert(body.end(), payload.data() + off, payload.data() + flen);
        if (body.size() > 256ull * 1024 * 1024) { *err = "h2: body too large"; return false; }
        /* replenish flow-control windows (stream + connection) */
        if (flen > 0) {
          uint32_t inc = (uint32_t)flen;
          uint8_t wu[13] = {0, 0, 4, H2_WINDOW_UPDATE, 0, 0, 0, 0, 0, 0, 0, 0, 0};
          wu[9] = (uint8_t)(inc >> 24); wu[10] = (uint8_t)(inc >> 16); wu[11] = (uint8_t)(inc >> 8); wu[12] = (uint8_t)inc;
          /* stream 1 */
          wu[8] = 1;
          if (!io.write((const char*)wu, 13)) { *err = "h2: window update failed"; return false; }
          /* connection (stream 0) */
          wu[8] = 0;
          if (!io.write((const char*)wu, 13)) { *err = "h2: window update failed"; return false; }
        }
        if (fflags & F_END_STREAM) goto done;
        break;
      }
      default:
        break; /* unknown frame types MUST be ignored (spec) */
    }
    if (ftype == H2_HEADERS && (fflags & F_END_STREAM) && saw_headers) goto done;
  }
done:
  if (!saw_headers) { *err = "h2: no response headers"; return false; }
  for (auto& kv : resp_headers) {
    if (kv.first == ":status") continue;
    res->headers.push_back({kv.first, kv.second});
  }
  res->status = status ? status : 0;
  res->body.assign(body.begin(), body.end());
  std::string enc = lower_copy(res->header("Content-Encoding"));
  if (enc.find("gzip") != std::string::npos || enc.find("deflate") != std::string::npos) {
    std::vector<uint8_t> inflated;
    if (gzip_inflate(res->body.data(), res->body.size(), &inflated)) res->body = std::move(inflated);
  }
  return true;
}
#endif /* !_WIN32 */

/* ============================ net_http_get ============================ */

HttpResult net_http_get(const std::string& url, const std::vector<HttpHeader>& extra_headers,
                        const std::string& method, int timeout_ms, CookieJar* jar,
                        const std::atomic<bool>* cancel) {
  HttpResult out;
  std::string current = url;
  for (int hop = 0; hop < 8; hop++) {
    std::string scheme, host, port, pq;
    if (!url_split(current, &scheme, &host, &port, &pq)) {
      out.err = "unparsable URL: " + current;
      return out;
    }
    bool is_tls = scheme == "https";
    if (scheme != "http" && !is_tls) { out.err = "unsupported scheme: " + scheme; return out; }

    /* build request */
    std::string req = method + " " + pq + " HTTP/1.1\r\n";
    req += "Host: " + host;
    if ((is_tls && port != "443") || (!is_tls && port != "80")) req += ":" + port;
    req += "\r\n";
    std::string ua = kDefaultUA;
    bool have_ua = false, have_accept = false, have_ae = false, have_conn = false;
    for (auto& h : extra_headers) {
      std::string ln = lower_copy(h.name);
      if (ln == "user-agent") { ua = h.value; have_ua = true; }
      if (ln == "accept") have_accept = true;
      if (ln == "accept-encoding") have_ae = true;
      if (ln == "connection") have_conn = true;
      req += h.name + ": " + h.value + "\r\n";
    }
    if (!have_ua) req += "User-Agent: " + ua + "\r\n";
    if (!have_accept) req += "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8\r\n";
    if (!have_ae) req += "Accept-Encoding: gzip, deflate\r\n";
    if (!have_conn) req += "Connection: close\r\n";
    req += "Upgrade-Insecure-Requests: 1\r\n";
    if (jar) {
      std::string ck = jar->get(host);
      if (!ck.empty()) req += "Cookie: " + ck + "\r\n";
    }
    req += "\r\n";

    TcpSocket sock;
    sock.fd = tcp_connect_timeout(host, port, timeout_ms, &out.err);
    if (!sock.valid()) return out;

#ifndef _WIN32
    if (is_tls) {
      TlsSession* tls = nullptr;
      bool h2_ok_flag = false;
      if (!https_session(sock, host, timeout_ms, &tls, &out.err, &h2_ok_flag)) return out;
      HttpResult res;
      bool okrd = false;
      if (getenv("SQRL_H2_DEBUG"))
        fprintf(stderr, "[h2] alpn h2 negotiated=%d\n", (int)h2_ok_flag);
      if (h2_ok_flag) {
        /* HTTP/2 over ALPN */
        bool h2ran = h2_perform(tls, method, host, pq, "https", extra_headers, jar, cancel,
                                timeout_ms, &res, &out.err);
        if (getenv("SQRL_H2_DEBUG"))
          fprintf(stderr, "[h2] negotiated=%d ran=%d err='%s' status=%d size=%zu\n",
                  (int)h2_ok_flag, (int)h2ran, out.err.c_str(), res.status, res.body.size());
        if (!h2ran) {
          /* h2 failed at the protocol level — retry once over HTTP/1.1 */
          delete tls;
          sock.close_fd(); /* close the h2 connection's raw socket */
          sock.fd = tcp_connect_timeout(host, port, timeout_ms, &out.err);
          if (!sock.valid()) return out;
          TcpSocket& sock2 = sock;
          TlsSession* tls2 = nullptr;
          if (!https_session(sock2, host, timeout_ms, &tls2, &out.err, nullptr)) return out;
          StreamOps io11;
          io11.read = [tls2](char* d, size_t n) { return tls_read(d, n, tls2); };
          io11.write = [tls2](const char* d, size_t n) { return tls_write(d, n, tls2); };
          if (!io11.write(req.data(), req.size())) { out.err = "send failed"; delete tls2; return out; }
          okrd = read_http_response(io11, &res, cancel);
          delete tls2;
          if (getenv("SQRL_H2_DEBUG"))
            fprintf(stderr, "[h2] fallback 1.1: okrd=%d status=%d size=%zu err='%s'\n",
                    (int)okrd, res.status, res.body.size(), res.err.c_str());
          if (!okrd) { out = res; return out; }
        } else {
          delete tls;
          okrd = true;
        }
      } else {
        StreamOps io;
        io.read = [tls](char* d, size_t n) { return tls_read(d, n, tls); };
        io.write = [tls](const char* d, size_t n) { return tls_write(d, n, tls); };
        if (!io.write(req.data(), req.size())) { out.err = "send failed"; delete tls; return out; }
        okrd = read_http_response(io, &res, cancel);
        delete tls;
        if (!okrd) { out = res; return out; }
      }
      std::string loc = res.header("Location");
      if ((res.status >= 301 && res.status <= 303) || res.status == 307 || res.status == 308) {
        if (!loc.empty()) { current = url_resolve(current, loc); continue; }
      }
      if (jar) {
        std::string hs = host;
        for (auto& h : res.headers) if (ieq(h.name, "Set-Cookie")) jar->store(hs, h.value);
      }
      res.ok = true;
      res.final_url = current;
      return res;
    }
#else
    if (is_tls) {
      SchannelStream* tls = new SchannelStream();
      tls->sock = sock.fd;
      if (!tls->init(host, &out.err)) { delete tls; return out; }
      StreamOps io;
      io.read = [tls](char* d, size_t n) { return tls->read(d, n); };
      io.write = [tls](const char* d, size_t n) { return tls->write(d, n); };
      if (!io.write(req.data(), req.size())) { out.err = "send failed: " + tls->last_err; delete tls; return out; }
      HttpResult res;
      bool okrd = read_http_response(io, &res, cancel);
      delete tls;
      if (!okrd) { out = res; return out; }
      std::string loc = res.header("Location");
      if ((res.status >= 301 && res.status <= 303) || res.status == 307 || res.status == 308) {
        if (!loc.empty()) { current = url_resolve(current, loc); continue; }
      }
      if (jar) for (auto& h : res.headers) if (ieq(h.name, "Set-Cookie")) jar->store(host, h.value);
      res.ok = true;
      res.final_url = current;
      return res;
    }
#endif

    /* plain HTTP */
    if (!send_all(sock.fd, req.data(), req.size())) { out.err = "send failed"; return out; }
    StreamOps io;
    io.read = [fd = sock.fd](char* d, size_t n) {
#ifdef _WIN32
      /* select-based read timeout (SO_RCVTIMEO unreliable on some systems) */
      fd_set rset;
      FD_ZERO(&rset);
      FD_SET((SOCKET)fd, &rset);
      struct timeval tv;
      tv.tv_sec = 30;
      tv.tv_usec = 0;
      if (select(fd + 1, &rset, nullptr, nullptr, &tv) <= 0) return -1;
#endif
      ssize_t r = recv(fd, d, n, 0);
      return (int)r;
    };
    io.write = [](const char*, size_t) { return true; };
    HttpResult res;
    if (!read_http_response(io, &res, cancel)) { out = res; return out; }
    std::string loc = res.header("Location");
    if ((res.status >= 301 && res.status <= 303) || res.status == 307 || res.status == 308) {
      if (!loc.empty()) { current = url_resolve(current, loc); continue; }
    }
    if (jar) for (auto& h : res.headers) if (ieq(h.name, "Set-Cookie")) jar->store(host, h.value);
    res.ok = true;
    res.final_url = current;
    return res;
  }
  out.err = "too many redirects";
  return out;
}

void net_init() {
#ifdef _WIN32
  static bool wsa_done = false;
  if (!wsa_done) {
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    wsa_done = true;
  }
#endif
#ifdef __linux__
  static std::once_flag flag;
  std::call_once(flag, []() {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);
  });
#endif
}

} /* namespace sqrl */
