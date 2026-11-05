// ta-mic-bridge: Tesla browser cabin mic -> ALSA loopback playback.
// WebSocket server on 127.0.0.1:<port> (lighttpd proxies wss://.../sockets/mic here).
// Binary client frames = PCM s16le mono 48 kHz. Written to the Loopback card
// (device 0 playback) so Android reads it from the Loopback capture side (device 1).
// Text frames {"type":"mic","state":"start|stop"} are logged; stop closes the PCM.
// Usage: ta-mic-bridge [port=9997] [channels=2]

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <tinyalsa/asoundlib.h>

#define RATE 48000
#define PERIOD 960 /* 20 ms */
#define PERIODS 4

static int g_channels = 2;
static struct pcm *g_pcm;
static unsigned long g_in_bytes, g_out_frames, g_errors;

static void logf_(const char *fmt, ...) {
  char ts[32];
  time_t t = time(NULL);
  strftime(ts, sizeof ts, "%H:%M:%S", localtime(&t));
  fprintf(stderr, "%s ta-mic: ", ts);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}
/* ---------- SHA1 + base64 for the WebSocket handshake ---------- */
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t n; } sha1_t;
#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
static void sha1_blk(sha1_t *s, const uint8_t *p) {
  uint32_t w[80], a, b, c, d, e, t;
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | p[4*i+1] << 16 | p[4*i+2] << 8 | p[4*i+3];
  for (int i = 16; i < 80; i++) w[i] = ROL(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
  a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
    else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
    else { f = b ^ c ^ d; k = 0xCA62C1D6; }
    t = ROL(a, 5) + f + e + k + w[i]; e = d; d = c; c = ROL(b, 30); b = a; a = t;
  }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
static void sha1(const uint8_t *d, size_t n, uint8_t out[20]) {
  sha1_t s = {{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0}, 0, {0}, 0};
  s.len = (uint64_t)n * 8;
  while (n >= 64) { sha1_blk(&s, d); d += 64; n -= 64; }
  uint8_t b[128] = {0};
  memcpy(b, d, n); b[n] = 0x80;
  size_t tot = (n < 56) ? 64 : 128;
  for (int i = 0; i < 8; i++) b[tot-1-i] = (uint8_t)(s.len >> (8*i));
  sha1_blk(&s, b); if (tot == 128) sha1_blk(&s, b + 64);
  for (int i = 0; i < 5; i++) { out[4*i] = s.h[i] >> 24; out[4*i+1] = s.h[i] >> 16; out[4*i+2] = s.h[i] >> 8; out[4*i+3] = s.h[i]; }
}
static void b64(const uint8_t *in, size_t n, char *out) {
  static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t i, o = 0;
  for (i = 0; i + 2 < n; i += 3) {
    uint32_t v = in[i] << 16 | in[i+1] << 8 | in[i+2];
    out[o++] = T[v >> 18]; out[o++] = T[(v >> 12) & 63]; out[o++] = T[(v >> 6) & 63]; out[o++] = T[v & 63];
  }
  if (i < n) {
    uint32_t v = in[i] << 16 | (i + 1 < n ? in[i+1] << 8 : 0);
    out[o++] = T[v >> 18]; out[o++] = T[(v >> 12) & 63];
    out[o++] = (i + 1 < n) ? T[(v >> 6) & 63] : '=';
    out[o++] = '=';
  }
  out[o] = 0;
}

/* ---------- ALSA loopback ---------- */
static int find_loopback_card(void) {
  FILE *f = fopen("/proc/asound/cards", "r");
  if (!f) return -1;
  char line[256]; int card = -1;
  while (fgets(line, sizeof line, f)) {
    int c; char id[64];
    if (sscanf(line, " %d [%63[^] ]", &c, id) == 2 && strcmp(id, "Loopback") == 0) { card = c; break; }
  }
  fclose(f);
  return card;
}
static void pcm_close_(void) { if (g_pcm) { pcm_close(g_pcm); g_pcm = NULL; logf_("pcm closed"); } }
static int pcm_open_(void) {
  if (g_pcm) return 0;
  int card = find_loopback_card();
  if (card < 0) { logf_("Loopback card not found (insmod snd-aloop.ko?)"); return -1; }
  struct pcm_config cfg = {0};
  cfg.channels = g_channels; cfg.rate = RATE; cfg.period_size = PERIOD; cfg.period_count = PERIODS;
  cfg.format = PCM_FORMAT_S16_LE; cfg.start_threshold = PERIOD; cfg.stop_threshold = PERIOD * PERIODS;
  g_pcm = pcm_open(card, 0, PCM_OUT, &cfg);
  if (!g_pcm || !pcm_is_ready(g_pcm)) {
    logf_("pcm_open card %d dev 0 failed: %s", card, g_pcm ? pcm_get_error(g_pcm) : "null");
    if (g_pcm) pcm_close(g_pcm);
    g_pcm = NULL; return -1;
  }
  logf_("pcm opened card %d dev 0, %dch %d Hz", card, g_channels, RATE);
  return 0;
}
static void write_pcm(const uint8_t *p, size_t n) {
  size_t frames = n / 2;
  if (!frames || pcm_open_() < 0) return;
  static int16_t out[PERIOD * PERIODS * 8];
  if (frames > PERIOD * PERIODS) frames = PERIOD * PERIODS;
  for (size_t i = 0; i < frames; i++) {
    int16_t s = (int16_t)(p[2*i] | p[2*i+1] << 8);
    for (int c = 0; c < g_channels; c++) out[i * g_channels + c] = s;
  }
  if (pcm_writei(g_pcm, out, frames) < 0) { g_errors++; logf_("pcm_writei: %s", pcm_get_error(g_pcm)); pcm_prepare(g_pcm); }
  else g_out_frames += frames;
}

/* ---------- WebSocket ---------- */
static int read_full(int fd, void *b, size_t n) {
  uint8_t *p = b;
  while (n) { ssize_t r = recv(fd, p, n, 0); if (r <= 0) return -1; p += r; n -= r; }
  return 0;
}
static int send_frame(int fd, int op, const void *d, size_t n) {
  uint8_t h[4] = {0x80 | op, (uint8_t)n};
  if (n > 125) return -1;
  if (send(fd, h, 2, MSG_NOSIGNAL) != 2) return -1;
  return n ? (send(fd, d, n, MSG_NOSIGNAL) == (ssize_t)n ? 0 : -1) : 0;
}
static int handshake(int fd) {
  char req[4096]; size_t n = 0;
  while (n < sizeof req - 1) {
    ssize_t r = recv(fd, req + n, sizeof req - 1 - n, 0);
    if (r <= 0) return -1;
    n += r; req[n] = 0;
    if (strstr(req, "\r\n\r\n")) break;
  }
  char *k = strcasestr(req, "Sec-WebSocket-Key:");
  if (!k) return -1;
  k += 18; while (*k == ' ') k++;
  char key[128]; int i = 0;
  while (*k && *k != '\r' && i < 100) key[i++] = *k++;
  key[i] = 0;
  strcat(key, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
  uint8_t dig[20]; char acc[64];
  sha1((uint8_t *)key, strlen(key), dig); b64(dig, 20, acc);
  char resp[256];
  int m = snprintf(resp, sizeof resp, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
  return send(fd, resp, m, MSG_NOSIGNAL) == m ? 0 : -1;
}
static void serve(int fd) {
  static uint8_t buf[1 << 16];
  if (handshake(fd) < 0) { logf_("handshake failed"); return; }
  logf_("client connected");
  time_t last = time(NULL);
  for (;;) {
    uint8_t h[2];
    if (read_full(fd, h, 2) < 0) break;
    int op = h[0] & 15, masked = h[1] & 0x80;
    uint64_t len = h[1] & 127;
    if (len == 126) { uint8_t e[2]; if (read_full(fd, e, 2) < 0) break; len = e[0] << 8 | e[1]; }
    else if (len == 127) { uint8_t e[8]; if (read_full(fd, e, 8) < 0) break; len = 0; for (int i = 0; i < 8; i++) len = len << 8 | e[i]; }
    uint8_t mask[4] = {0};
    if (masked && read_full(fd, mask, 4) < 0) break;
    if (len > sizeof buf) { logf_("frame too large %llu", (unsigned long long)len); break; }
    if (read_full(fd, buf, len) < 0) break;
    if (masked) for (uint64_t i = 0; i < len; i++) buf[i] ^= mask[i & 3];
    if (op == 8) break;
    if (op == 9) { send_frame(fd, 10, buf, len > 125 ? 0 : len); continue; }
    if (op == 1) {
      buf[len < sizeof buf ? len : sizeof buf - 1] = 0;
      logf_("text: %s", (char *)buf);
      if (strstr((char *)buf, "\"stop\"")) pcm_close_();
    } else if (op == 2 || op == 0) {
      g_in_bytes += len;
      write_pcm(buf, len);
    }
    if (time(NULL) - last >= 10) {
      last = time(NULL);
      logf_("stats in=%lu B out=%lu frames err=%lu", g_in_bytes, g_out_frames, g_errors);
    }
  }
  pcm_close_();
  logf_("client closed");
}

int main(int argc, char **argv) {
  int port = argc > 1 ? atoi(argv[1]) : 9997;
  if (argc > 2) g_channels = atoi(argv[2]);
  signal(SIGPIPE, SIG_IGN);
  int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 2) < 0) { perror("bind/listen"); return 1; }
  logf_("listening 127.0.0.1:%d, %d ch", port, g_channels);
  for (;;) {
    int c = accept(s, NULL, NULL);
    if (c < 0) continue;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    serve(c);
    close(c);
  }
}
