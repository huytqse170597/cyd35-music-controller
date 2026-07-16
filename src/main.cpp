#include <SPI.h>
#include <TFT_eSPI.h>
#include <U8g2_for_TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TJpg_Decoder.h>
#include <WebSocketsClient.h>
#include <Preferences.h>
#include <SD.h>
#include "secrets.h" // WIFI_SSID, WIFI_PASS, API_HOST, API_PORT, API_BASE — see secrets.h.example

// SD card is on its own SPI bus (VSPI: CS=5, SCK=18, MISO=19, MOSI=23),
// separate from the TFT's HSPI bus — no bus-sharing/frequency conflict with
// the display or touch controller.
#define SD_CS_PIN 5

TFT_eSPI tft = TFT_eSPI();
U8g2_for_TFT_eSPI u8f;
WebSocketsClient webSocket;
Preferences prefs;
uint16_t calData[5];
TFT_eSprite titleSprite = TFT_eSprite(&tft);
TFT_eSprite artistSprite = TFT_eSprite(&tft);

uint16_t COL_BG, COL_TRACK, COL_ART, COL_ICON;

// layout: album art left, title/artist/progress/controls stacked right + below
#define ART_X 20
#define ART_Y 20
#define ART_SIZE 190

#define COL_X (ART_X + ART_SIZE + 20)
#define COL_W (480 - COL_X - 20)
#define TITLE_BOX_Y (TITLE_Y - 18)
#define TITLE_Y 55
#define ARTIST_BOX_Y (ARTIST_Y - 18)
#define ARTIST_Y 92
#define TEXT_BOX_H 24
#define TEXT_BASELINE 18

#define BAR_X ART_X
#define BAR_Y (ART_Y + ART_SIZE + 15)
#define BAR_W (COL_X + COL_W - ART_X)
#define BAR_H 3
#define TIME_Y (BAR_Y + 7)
#define TIME_LABEL_W 50
#define TIME_LABEL_H 14

#define CTRL_Y 250
#define CTRL_H 60
#define CTRL_CY (CTRL_Y + CTRL_H / 2)
#define ZONE_X BAR_X
#define ZONE_W BAR_W
#define PREV_CX (ZONE_X + ZONE_W / 6)
#define PLAY_CX (ZONE_X + ZONE_W / 2)
#define NEXT_CX (ZONE_X + ZONE_W * 5 / 6)

#define MARQUEE_GAP 40
#define MARQUEE_STEP_PX 2
#define MARQUEE_INTERVAL_MS 50
#define POSITION_DEBOUNCE_MS 500

#define LONG_PRESS_MS 550
#define VOLUME_DRAG_THRESHOLD_PX 12
#define VOLUME_DRAG_RANGE_PX 180
#define VOLUME_POST_INTERVAL_MS 120

#define MAX_ART_JPEG_BYTES 14000

#define QUEUE_HEADER_H 34
#define QUEUE_ROW_H 34
#define QUEUE_PAGER_W 40
#define QUEUE_VISIBLE_ROWS ((320 - QUEUE_HEADER_H) / QUEUE_ROW_H)
#define QUEUE_TAB_X COL_X
#define QUEUE_TAB_Y (ARTIST_BOX_Y + TEXT_BOX_H + 10)
#define QUEUE_TAB_W 110
#define QUEUE_TAB_H 34

#define MODE_TAB_Y (QUEUE_TAB_Y + QUEUE_TAB_H + 8)
#define MODE_TAB_H 34
#define SHUFFLE_TAB_X COL_X
#define SHUFFLE_TAB_W 64
#define REPEAT_TAB_X (SHUFFLE_TAB_X + SHUFFLE_TAB_W + 8)
#define REPEAT_TAB_W 64

String lastTitle = "";
String lastArtist = "";
int titleWidthPx = 0;
int artistWidthPx = 0;
int titleScrollX = 0;
int artistScrollX = 0;
unsigned long lastScrollMs = 0;

bool isPlaying = false;
float currentDuration = 0;
float currentPosition = 0;
unsigned long lastTouchMs = 0;
unsigned long ignorePositionUntilMs = 0;

int lastFillW = 0;
String lastElapsedLabel = "";
String lastDurationLabel = "";

// like/volume/seek state (api-server extras — see docs/api.md in pear-desktop)
String likeState = "";
int currentVolume = 50;
bool isMuted = false;
bool currentShuffle = false;
String currentRepeatMode = "NONE";

bool touchWasDown = false;
uint16_t touchStartX = 0, touchStartY = 0;
unsigned long touchStartMs = 0;
bool touchIsDragVolume = false;
bool touchLikeFired = false;
bool touchBarSeekFired = false;
int dragStartVolume = 0;
unsigned long lastVolumePostMs = 0;
int lastSentVolume = -1;
int queueLongPressRow = -1;
bool queueRemoveFired = false;

// Momentary press-highlight: a white ring flashed on the tapped control so a
// tap registers instantly, instead of only "confirming" once the server's
// state change round-trips back over the websocket. Queue-tab/seek are left
// out — those give their own feedback (screen change / bar jump) immediately.
enum PressBtn { PB_NONE, PB_PREV, PB_PLAY, PB_NEXT, PB_SHUFFLE, PB_REPEAT };
PressBtn flashBtn = PB_NONE;
unsigned long flashClearMs = 0;
#define PRESS_FLASH_MS 130

// XPT2046 has no separate "connection lost" signal — this is purely the
// websocket's own connect/disconnect events. No reconnect timer is drawn:
// WebSocketsClient already auto-reconnects (setReconnectInterval in setup()).
bool wsConnected = false;

const uint8_t *titleFont = u8g2_font_unifont_t_vietnamese2;
const uint8_t *artistFont = u8g2_font_unifont_t_vietnamese2;

// Cached so the art can be repainted (like-badge redraw, returning from the
// queue screen) without re-requesting it — the websocket only pushes a new
// frame on song/art change, not on demand.
static uint8_t lastArtJpeg[MAX_ART_JPEG_BYTES];
static size_t lastArtJpegLen = 0;

enum Screen { SCREEN_PLAYER, SCREEN_QUEUE };
Screen currentScreen = SCREEN_PLAYER;

JsonDocument queueDoc;
int queueItemCount = 0;
int queueSelectedIndex = -1;
int queueScrollOffset = 0;

// Art cache on SD, keyed by videoId — lets a replayed song show its art
// immediately (from a prior fetch) instead of a placeholder flash while
// waiting for the websocket to push a fresh frame.
bool sdAvailable = false;
String currentVideoId = "";

// Picks a font per-string instead of one global font: the Vietnamese unifont
// subset has no CJK glyphs and the Japanese subset (u8g2_font_unifont_t_japanese1,
// ~3000 common Kanji + Hiragana/Katakana) is a separate ROM to keep flash usage
// down. ponytail: only the first ~3000 Kanji are covered, not all of them —
// rare characters render blank; add japanese2/3 with per-glyph fallback if that
// turns out to matter in practice.
const uint8_t *pickFont(const String &text) {
  const uint8_t *bytes = (const uint8_t *)text.c_str();
  size_t n = text.length();
  size_t i = 0;
  while (i < n) {
    uint8_t b0 = bytes[i];
    uint32_t cp;
    int len;
    if (b0 < 0x80) {
      cp = b0;
      len = 1;
    } else if ((b0 & 0xE0) == 0xC0 && i + 1 < n) {
      cp = ((uint32_t)(b0 & 0x1F) << 6) | (bytes[i + 1] & 0x3F);
      len = 2;
    } else if ((b0 & 0xF0) == 0xE0 && i + 2 < n) {
      cp = ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(bytes[i + 1] & 0x3F) << 6) | (bytes[i + 2] & 0x3F);
      len = 3;
    } else if ((b0 & 0xF8) == 0xF0 && i + 3 < n) {
      cp = ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(bytes[i + 1] & 0x3F) << 12) |
           ((uint32_t)(bytes[i + 2] & 0x3F) << 6) | (bytes[i + 3] & 0x3F);
      len = 4;
    } else {
      cp = b0;
      len = 1;
    }
    bool isHiraganaKatakana = cp >= 0x3040 && cp <= 0x30FF;
    bool isKanji = cp >= 0x4E00 && cp <= 0x9FFF;
    if (isHiraganaKatakana || isKanji) return u8g2_font_unifont_t_japanese1;
    i += len;
  }
  return u8g2_font_unifont_t_vietnamese2;
}

void drawArtPlaceholder() {
  tft.fillRoundRect(ART_X, ART_Y, ART_SIZE, ART_SIZE, 14, COL_ART);
  // ponytail: music-note icon shown until the first art frame arrives over
  // the WS (or if the server sends an empty frame, e.g. song has no art).
  int cx = ART_X + ART_SIZE / 2;
  int cy = ART_Y + ART_SIZE / 2;
  tft.fillCircle(cx - 20, cy + 32, 15, TFT_WHITE);
  tft.fillCircle(cx + 24, cy + 22, 15, TFT_WHITE);
  tft.fillRect(cx + 10, cy - 52, 6, 84, TFT_WHITE);
  tft.fillRect(cx - 35, cy - 42, 6, 84, TFT_WHITE);
  tft.fillTriangle(cx - 35, cy - 42, cx + 10, cy - 52, cx + 10, cy - 36, TFT_WHITE);
  tft.fillTriangle(cx - 35, cy - 42, cx - 35, cy - 26, cx + 10, cy - 36, TFT_WHITE);
}

bool tftOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  tft.pushImage(x, y, w, h, bitmap);
  return true;
}

void drawHeart(int cx, int cy, int r, uint16_t color) {
  tft.fillCircle(cx - r / 2, cy - r / 3, r / 2, color);
  tft.fillCircle(cx + r / 2, cy - r / 3, r / 2, color);
  tft.fillTriangle(cx - r, cy - r / 4, cx + r, cy - r / 4, cx, cy + r, color);
}

// Like badge, top-right corner of the art box — drawn straight over the art
// with no backing/outline, so an un-liked song shows nothing at all (just
// the art, untouched) instead of an always-visible empty-heart badge.
void drawLikeIcon() {
  if (likeState != "LIKE") return;
  int cx = ART_X + ART_SIZE - 20;
  int cy = ART_Y + 18;
  uint16_t heartColor = tft.color565(255, 40, 90); // pink-red
  drawHeart(cx, cy, 10, heartColor);
}

// SD cache status, top-left corner of the art box (mirrors the like badge on
// the top-right) — green when mounted, dim gray otherwise. Checked once at
// boot only, not re-checked live if a card is hot-swapped mid-session.
void drawSdIndicator() {
  int cx = ART_X + 14;
  int cy = ART_Y + 14;
  uint16_t color = sdAvailable ? tft.color565(60, 200, 90) : COL_ICON;
  tft.fillCircle(cx, cy, 6, color);
}

// Bottom-left corner of the art box — only drawn while actually disconnected,
// same "show nothing when there's nothing to say" idea as the like badge.
void drawWsIndicator() {
  if (wsConnected) return;
  int cx = ART_X + 14;
  int cy = ART_Y + ART_SIZE - 14;
  tft.fillCircle(cx, cy, 6, TFT_RED);
}

void drawArtBadges() {
  drawLikeIcon();
  drawSdIndicator();
  drawWsIndicator();
}

// Art arrives pre-scaled to ART_SIZE and already re-encoded as a small JPEG
// by the desktop app's api-server plugin (see websocket.ts broadcastAlbumArt) —
// this device only decodes, it never fetches or resizes the original thumbnail.
void drawAlbumArtJpeg(uint8_t *payload, size_t length) {
  if (length == 0) {
    drawArtPlaceholder();
  } else {
    uint16_t jw = 0, jh = 0;
    TJpgDec.getJpgSize(&jw, &jh, payload, length);
    // Only clear first when the decoded image won't cover the whole box
    // (letterboxed / portrait art). For the normal 190x190 frame the decode
    // overwrites every pixel, so skipping the clear removes the dark flash
    // that made each song change look like it "scanned" in.
    if (jw < ART_SIZE || jh < ART_SIZE) tft.fillRect(ART_X, ART_Y, ART_SIZE, ART_SIZE, COL_BG);
    int ox = ART_X + (ART_SIZE - (int)jw) / 2;
    int oy = ART_Y + (ART_SIZE - (int)jh) / 2;
    TJpgDec.drawJpg(ox, oy, payload, length);
  }
  drawArtBadges();
}

// Repaints the art box from the cached JPEG (or placeholder) — used when the
// like state changes without a fresh art frame, and when returning from the
// queue screen. Never call this from drawArtBadges()/drawAlbumArtJpeg() itself
// (drawAlbumArtJpeg already ends by calling drawArtBadges()) — that would recurse.
void redrawArt() {
  if (lastArtJpegLen > 0) {
    drawAlbumArtJpeg(lastArtJpeg, lastArtJpegLen);
  } else {
    drawArtPlaceholder();
    drawArtBadges();
  }
}

// Loads a previously-cached frame straight into lastArtJpeg — does not draw
// it, callers decide whether/when to redrawArt(). ponytail: no eviction —
// JPEGs here are capped at MAX_ART_JPEG_BYTES each, so even a few thousand
// unique songs is a few tens of MB, trivial for a real SD card.
bool loadArtFromCache(const String &videoId) {
  if (!sdAvailable || videoId.length() == 0) return false;

  File f = SD.open("/art/" + videoId + ".jpg", FILE_READ);
  if (!f) return false;

  size_t len = f.size();
  bool ok = len > 0 && len <= sizeof(lastArtJpeg);
  if (ok) {
    f.read(lastArtJpeg, len);
    lastArtJpegLen = len;
  }
  f.close();
  return ok;
}

// Write-through, once per song: if a cache file already exists for this
// videoId there's nothing to do (art doesn't change for a given song).
void saveArtToCache(const String &videoId, uint8_t *payload, size_t length) {
  if (!sdAvailable || videoId.length() == 0 || length == 0) return;

  String path = "/art/" + videoId + ".jpg";
  if (SD.exists(path)) return;

  File f = SD.open(path, FILE_WRITE);
  if (!f) return;
  f.write(payload, length);
  f.close();
}

void drawPlayPauseIcon(bool paused) {
  tft.fillRect(PLAY_CX - 30, CTRL_Y, 60, CTRL_H, COL_BG);
  if (paused) {
    tft.fillTriangle(PLAY_CX - 14, CTRL_CY - 20, PLAY_CX - 14, CTRL_CY + 20, PLAY_CX + 18, CTRL_CY, TFT_WHITE);
  } else {
    tft.fillRoundRect(PLAY_CX - 18, CTRL_CY - 20, 12, 40, 2, TFT_WHITE);
    tft.fillRoundRect(PLAY_CX + 6, CTRL_CY - 20, 12, 40, 2, TFT_WHITE);
  }
}

void drawPrevNextIcons() {
  tft.fillTriangle(PREV_CX + 14, CTRL_CY - 16, PREV_CX + 14, CTRL_CY + 16, PREV_CX - 10, CTRL_CY, COL_ICON);
  tft.fillRect(PREV_CX - 16, CTRL_CY - 16, 5, 32, COL_ICON);

  tft.fillTriangle(NEXT_CX - 14, CTRL_CY - 16, NEXT_CX - 14, CTRL_CY + 16, NEXT_CX + 10, CTRL_CY, COL_ICON);
  tft.fillRect(NEXT_CX + 11, CTRL_CY - 16, 5, 32, COL_ICON);
}

// Tappable "Queue" tab — sits in the otherwise-empty gap between the
// artist line and the progress bar, right of the album art.
void drawQueueTab() {
  tft.fillRoundRect(QUEUE_TAB_X, QUEUE_TAB_Y, QUEUE_TAB_W, QUEUE_TAB_H, 6, COL_TRACK);
  int iy = QUEUE_TAB_Y + QUEUE_TAB_H / 2;
  for (int i = 0; i < 3; i++) tft.fillRect(QUEUE_TAB_X + 10, iy - 8 + i * 7, 16, 3, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, COL_TRACK);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Queue", QUEUE_TAB_X + 34, iy, 2);
  tft.setTextDatum(TL_DATUM);
}

// Shuffle: on/off, green when on. Repeat: off/all/one — label switches to
// "REP1" for the one-song case, green background for either "on" state,
// since a single word can't carry three states on its own at this size.
void drawModeButtons() {
  uint16_t sBg = currentShuffle ? tft.color565(60, 200, 90) : COL_TRACK;
  tft.fillRoundRect(SHUFFLE_TAB_X, MODE_TAB_Y, SHUFFLE_TAB_W, MODE_TAB_H, 6, sBg);
  tft.setTextColor(TFT_WHITE, sBg);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("SHUF", SHUFFLE_TAB_X + SHUFFLE_TAB_W / 2, MODE_TAB_Y + MODE_TAB_H / 2, 2);

  uint16_t rBg = (currentRepeatMode == "NONE") ? COL_TRACK : tft.color565(60, 200, 90);
  String rLabel = (currentRepeatMode == "ONE") ? "REP1" : "REP";
  tft.fillRoundRect(REPEAT_TAB_X, MODE_TAB_Y, REPEAT_TAB_W, MODE_TAB_H, 6, rBg);
  tft.setTextColor(TFT_WHITE, rBg);
  tft.drawString(rLabel, REPEAT_TAB_X + REPEAT_TAB_W / 2, MODE_TAB_Y + MODE_TAB_H / 2, 2);
  tft.setTextDatum(TL_DATUM);
}

void pressButtonRect(PressBtn b, int &x, int &y, int &w, int &h) {
  switch (b) {
    case PB_PREV: x = PREV_CX - 30; y = CTRL_Y; w = 60; h = CTRL_H; break;
    case PB_PLAY: x = PLAY_CX - 30; y = CTRL_Y; w = 60; h = CTRL_H; break;
    case PB_NEXT: x = NEXT_CX - 30; y = CTRL_Y; w = 60; h = CTRL_H; break;
    case PB_SHUFFLE: x = SHUFFLE_TAB_X; y = MODE_TAB_Y; w = SHUFFLE_TAB_W; h = MODE_TAB_H; break;
    case PB_REPEAT: x = REPEAT_TAB_X; y = MODE_TAB_Y; w = REPEAT_TAB_W; h = MODE_TAB_H; break;
    default: x = y = w = h = 0;
  }
}

// A 2px white ring, restored by the button's own normal-draw once the timer
// expires — no cached framebuffer, the existing draw functions already know
// how to repaint each button.
void flashPress(PressBtn b) {
  if (flashBtn != b) {
    int x, y, w, h;
    pressButtonRect(b, x, y, w, h);
    tft.drawRoundRect(x, y, w, h, 6, TFT_WHITE);
    tft.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 6, TFT_WHITE);
    flashBtn = b;
  }
  flashClearMs = millis() + PRESS_FLASH_MS;
}

void clearPressFlash() {
  PressBtn b = flashBtn;
  flashBtn = PB_NONE;
  if (currentScreen != SCREEN_PLAYER) return; // screen already fully repainted elsewhere
  switch (b) {
    case PB_PREV:
    case PB_NEXT: {
      int x, y, w, h;
      pressButtonRect(b, x, y, w, h);
      tft.fillRect(x, y, w, h, COL_BG);
      drawPrevNextIcons();
      break;
    }
    case PB_PLAY: drawPlayPauseIcon(!isPlaying); break;
    case PB_SHUFFLE:
    case PB_REPEAT: drawModeButtons(); break;
    default: break;
  }
}

// withPlaceholder=false when the caller repaints art itself right after (accent
// change, returning from queue) — drawing the dark music-note placeholder there
// only to overwrite it a moment later is the note-flash seen each song change.
void drawStaticUI(bool withPlaceholder = true) {
  tft.resetViewport();
  tft.fillScreen(COL_BG);
  if (withPlaceholder) drawArtPlaceholder();
  drawArtBadges();
  drawQueueTab();
  drawModeButtons();
  // Full-length track drawn up front — it used to only appear piecemeal as
  // updateProgressBar() painted deltas, so it never showed its full length
  // until progress had already grown across the whole thing.
  tft.fillRect(BAR_X, BAR_Y, BAR_W, BAR_H, COL_TRACK);
  lastFillW = 0;
  drawPrevNextIcons();
  drawPlayPauseIcon(!isPlaying);
}

// Only paint the delta since the last draw (no clear+redraw of the whole
// bar) — position ticks arrive ~1/s and a full clear+fill every tick is what
// reads as screen flicker.
void updateProgressBar() {
  int fillW = 0;
  if (currentDuration > 0) {
    fillW = (int)(BAR_W * (currentPosition / currentDuration));
    if (fillW < 0) fillW = 0;
    if (fillW > BAR_W) fillW = BAR_W;
  }

  if (fillW != lastFillW) {
    if (fillW > lastFillW) {
      tft.fillRect(BAR_X + lastFillW, BAR_Y, fillW - lastFillW, BAR_H, TFT_WHITE);
    } else {
      // Song changed / seek backward: reset the whole track first instead of
      // just the shrunk segment, so no stray sliver can be left over.
      tft.fillRect(BAR_X, BAR_Y, BAR_W, BAR_H, COL_TRACK);
      if (fillW > 0) tft.fillRect(BAR_X, BAR_Y, fillW, BAR_H, TFT_WHITE);
    }
    lastFillW = fillW;
  }
}

String formatTime(float seconds) {
  if (seconds < 0) seconds = 0;
  int total = (int)seconds;
  int m = total / 60;
  int s = total % 60;
  char buf[8];
  snprintf(buf, sizeof(buf), "%d:%02d", m, s);
  return String(buf);
}

void updateTimeLabels() {
  String elapsedStr = formatTime(currentPosition);
  String durationStr = formatTime(currentDuration);

  if (elapsedStr != lastElapsedLabel) {
    lastElapsedLabel = elapsedStr;
    tft.fillRect(BAR_X, TIME_Y, TIME_LABEL_W, TIME_LABEL_H, COL_BG);
    tft.setTextColor(TFT_LIGHTGREY, COL_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(elapsedStr, BAR_X, TIME_Y, 2);
  }
  if (durationStr != lastDurationLabel) {
    lastDurationLabel = durationStr;
    tft.fillRect(BAR_X + BAR_W - TIME_LABEL_W, TIME_Y, TIME_LABEL_W, TIME_LABEL_H, COL_BG);
    tft.setTextColor(TFT_LIGHTGREY, COL_BG);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(durationStr, BAR_X + BAR_W, TIME_Y, 2);
  }
}

// Reuses the progress-bar/time row for a transient "Volume NN%" readout
// while dragging, instead of caching art to overlay it elsewhere.
void drawVolumeOverlay(int percent) {
  tft.fillRect(BAR_X, BAR_Y - 2, BAR_W, (TIME_Y + TIME_LABEL_H) - (BAR_Y - 2), COL_BG);
  tft.setTextColor(TFT_WHITE, COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Volume " + String(percent) + "%", BAR_X + BAR_W / 2, BAR_Y + 10, 2);
  tft.setTextDatum(TL_DATUM);
}

void clearVolumeOverlay() {
  tft.fillRect(BAR_X, BAR_Y - 2, BAR_W, (TIME_Y + TIME_LABEL_H) - (BAR_Y - 2), COL_BG);
  tft.fillRect(BAR_X, BAR_Y, BAR_W, BAR_H, COL_TRACK);
  lastFillW = 0;
  lastElapsedLabel = "\x01";
  lastDurationLabel = "\x01";
  updateProgressBar();
  updateTimeLabels();
}

// Draws the title/artist box relative to its own viewport: setViewport()
// clips (and shifts the coordinate origin) so scrolled/oversized text never
// paints outside the box — this is what used to leave stray pixels behind
// once a shorter string replaced a longer one.
//
// Drawn into an off-screen sprite first, then blitted in one pushSprite()
// call — drawing the clear+text directly to the panel every scroll tick
// was visible as flicker (each glyph is its own small SPI write, so the
// panel briefly shows the cleared background between them).
void renderTextBox(TFT_eSprite &sprite, int boxX, int boxY, const String &text, int textWidth, int scrollX,
                    uint16_t color, const uint8_t *font) {
  sprite.fillSprite(COL_BG);
  u8f.begin(sprite);
  u8f.setForegroundColor(color);
  u8f.setFont(font);
  // setFont() resets the decoder to opaque mode (is_transparent=0), which
  // paints each glyph's background box in the unset bg_color (black) — the
  // black rectangles seen around characters. Transparent mode draws only the
  // glyph pixels, letting the sprite's COL_BG fill show through.
  u8f.setFontMode(1);

  if (textWidth <= COL_W) {
    u8f.setCursor(0, TEXT_BASELINE);
    u8f.print(text);
  } else {
    int period = textWidth + MARQUEE_GAP;
    int x1 = -scrollX;
    u8f.setCursor(x1, TEXT_BASELINE);
    u8f.print(text);
    int x2 = x1 + period;
    if (x2 < COL_W) {
      u8f.setCursor(x2, TEXT_BASELINE);
      u8f.print(text);
    }
  }
  u8f.begin(tft);
  sprite.pushSprite(boxX, boxY);
}

void renderTitleBox() {
  renderTextBox(titleSprite, COL_X, TITLE_BOX_Y, lastTitle, titleWidthPx, titleScrollX, TFT_WHITE, titleFont);
}
void renderArtistBox() {
  renderTextBox(artistSprite, COL_X, ARTIST_BOX_Y, lastArtist, artistWidthPx, artistScrollX, TFT_LIGHTGREY, artistFont);
}

// Metrics (font/width) are always recomputed so state stays correct even
// while the queue screen is showing; the sprite is only repainted when the
// player screen is actually visible (see currentScreen guards in the caller).
void setTitleText(const String &text) {
  lastTitle = text;
  titleFont = pickFont(text);
  u8f.setFont(titleFont);
  titleWidthPx = u8f.getUTF8Width(text.c_str());
  titleScrollX = 0;
  if (currentScreen == SCREEN_PLAYER) renderTitleBox();
}

void setArtistText(const String &text) {
  lastArtist = text;
  artistFont = pickFont(text);
  u8f.setFont(artistFont);
  artistWidthPx = u8f.getUTF8Width(text.c_str());
  artistScrollX = 0;
  if (currentScreen == SCREEN_PLAYER) renderArtistBox();
}

void updateMarquee() {
  if (millis() - lastScrollMs < MARQUEE_INTERVAL_MS) return;
  lastScrollMs = millis();

  if (titleWidthPx > COL_W) {
    titleScrollX += MARQUEE_STEP_PX;
    if (titleScrollX >= titleWidthPx + MARQUEE_GAP) titleScrollX = 0;
    renderTitleBox();
  }
  if (artistWidthPx > COL_W) {
    artistScrollX += MARQUEE_STEP_PX;
    if (artistScrollX >= artistWidthPx + MARQUEE_GAP) artistScrollX = 0;
    renderArtistBox();
  }
}

// Scales the (already dark-tinted-friendly) accent color down so it always
// works as a dark background regardless of the source hue, so a single
// white foreground stays legible without per-color contrast math.
uint16_t rgb565FromHexScaled(const char *hex, float scale) {
  if (strlen(hex) < 7 || hex[0] != '#') return tft.color565(20, 20, 20);
  long val = strtol(hex + 1, nullptr, 16);
  int r = (int)(((val >> 16) & 0xFF) * scale);
  int g = (int)(((val >> 8) & 0xFF) * scale);
  int b = (int)((val & 0xFF) * scale);
  if (r > 255) r = 255;
  if (g > 255) g = 255;
  if (b > 255) b = 255;
  return tft.color565(r, g, b);
}

void applyAccentColor(const char *hex) {
  if (hex[0] == 0) return;
  COL_BG = rgb565FromHexScaled(hex, 0.28f);
  COL_ART = rgb565FromHexScaled(hex, 0.45f);
  COL_TRACK = rgb565FromHexScaled(hex, 0.55f);

  // Colors are still recorded above even off-screen — only the repaint is
  // skipped, and closeQueueScreen()/renderNowPlayingScreen() picks them up
  // whenever the player screen becomes visible again.
  if (currentScreen != SCREEN_PLAYER) return;

  drawStaticUI(false);
  redrawArt();
  renderTitleBox();
  renderArtistBox();

  lastElapsedLabel = "\x01";
  lastDurationLabel = "\x01";
  updateProgressBar();
  updateTimeLabels();
}

// Shared by every REST call: builds the URL into a fixed buffer (no per-call
// `String(API_BASE) + path` heap alloc — that String churn on the touch/tick
// hot paths is the main fragmentation source over long uptime), and caps
// connect+read timeouts so a dead/unreachable server can't freeze loop() for
// the multi-second default. urlBuf is safe to share: only one HTTP call runs
// at a time in the single-threaded loop.
static char urlBuf[160];
void httpBegin(HTTPClient &http, const char *path) {
  snprintf(urlBuf, sizeof(urlBuf), "%s%s", API_BASE, path);
  http.begin(urlBuf);
  http.setConnectTimeout(1000);
  http.setTimeout(1000);
}

void postAction(const char *path) {
  HTTPClient http;
  httpBegin(http, path);
  http.POST("");
  http.end();
}

void postActionJson(const char *path, const String &jsonBody) {
  HTTPClient http;
  httpBegin(http, path);
  http.addHeader("Content-Type", "application/json");
  http.POST(jsonBody);
  http.end();
}

void patchActionJson(const char *path, const String &jsonBody) {
  HTTPClient http;
  httpBegin(http, path);
  http.addHeader("Content-Type", "application/json");
  http.sendRequest("PATCH", jsonBody);
  http.end();
}

// Only endpoint that isn't pushed over the websocket — polled once per song
// so the heart badge reflects state that may have been set from another
// client, not just this device's own taps.
void fetchLikeState() {
  HTTPClient http;
  httpBegin(http, "/api/v1/like-state");
  int code = http.GET();
  if (code == 200) {
    JsonDocument doc;
    if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
      likeState = String((const char *)(doc["state"] | ""));
    }
  }
  http.end();
  if (currentScreen == SCREEN_PLAYER) redrawArt();
}

String queueItemTitle(int i) {
  const char *t = queueDoc["items"][i]["title"] | "";
  return String(t);
}

String queueItemArtist(int i) {
  const char *t = queueDoc["items"][i]["artist"] | "";
  return String(t);
}

// `/api/v1/queue` is a raw pass-through of YouTube Music's internal queue
// object — for a real queue this runs several hundred KB (menus, tracking
// params, multiple thumbnail sizes per item), far too much to fetch/parse on
// an ESP32. /api/v1/queue/list is the slimmed server-side equivalent
// ({title, artist, videoId, selected}[] only), so this can just be a plain
// GET + parse.
void fetchQueue() {
  queueItemCount = 0;
  queueSelectedIndex = -1;
  queueDoc.clear();

  HTTPClient http;
  httpBegin(http, "/api/v1/queue/list");
  int code = http.GET();
  if (code == 200 && deserializeJson(queueDoc, http.getString()) == DeserializationError::Ok) {
    queueItemCount = queueDoc["items"].as<JsonArray>().size();
    for (int i = 0; i < queueItemCount; i++) {
      if (queueDoc["items"][i]["selected"] | false) queueSelectedIndex = i;
    }
  }
  http.end();
}

// Whole-page jumps, not drag-scroll: dragging redraws up to 8 rows of
// u8g2 text per touch sample, which visibly lagged (font rendering over SPI
// isn't cheap). A button redraws once per tap instead.
void drawQueuePager() {
  int px = 480 - QUEUE_PAGER_W;
  int totalH = 320 - QUEUE_HEADER_H;
  int halfH = totalH / 2;
  tft.fillRect(px, QUEUE_HEADER_H, QUEUE_PAGER_W, totalH, COL_TRACK);

  bool canUp = queueScrollOffset > 0;
  bool canDown = queueScrollOffset + QUEUE_VISIBLE_ROWS < queueItemCount;
  int cx = px + QUEUE_PAGER_W / 2;

  int upCy = QUEUE_HEADER_H + halfH / 2;
  tft.fillTriangle(cx - 10, upCy + 6, cx + 10, upCy + 6, cx, upCy - 8, canUp ? TFT_WHITE : COL_ICON);

  int downCy = QUEUE_HEADER_H + halfH + halfH / 2;
  tft.fillTriangle(cx - 10, downCy - 6, cx + 10, downCy - 6, cx, downCy + 8, canDown ? TFT_WHITE : COL_ICON);
}

// Each row is clipped via setViewport rather than truncated by hand — same
// idiom renderTextBox uses for the marquee, reused here for a static line.
void drawQueueRows() {
  int listW = 480 - QUEUE_PAGER_W;
  tft.fillRect(0, QUEUE_HEADER_H, listW, 320 - QUEUE_HEADER_H, COL_BG);

  if (queueItemCount == 0) {
    tft.setTextColor(TFT_LIGHTGREY, COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Queue empty", listW / 2, QUEUE_HEADER_H + 40, 2);
    tft.setTextDatum(TL_DATUM);
    return;
  }

  for (int row = 0; row < QUEUE_VISIBLE_ROWS; row++) {
    int idx = queueScrollOffset + row;
    if (idx >= queueItemCount) break;
    int ry = QUEUE_HEADER_H + row * QUEUE_ROW_H;
    bool selected = idx == queueSelectedIndex;

    tft.fillRect(0, ry, listW, QUEUE_ROW_H, selected ? COL_ART : COL_BG);

    String line = queueItemTitle(idx) + "  \xC2\xB7  " + queueItemArtist(idx);
    tft.setViewport(0, ry, listW, QUEUE_ROW_H);
    u8f.begin(tft);
    u8f.setForegroundColor(selected ? TFT_WHITE : TFT_LIGHTGREY);
    u8f.setFont(pickFont(line));
    u8f.setFontMode(1); // transparent — see renderTextBox note (no black glyph boxes)
    u8f.setCursor(12, 22);
    u8f.print(line);
    tft.resetViewport();
  }
  u8f.begin(tft);
}

void drawQueueScreen() {
  tft.resetViewport();
  tft.fillScreen(COL_BG);
  tft.fillRect(0, 0, 480, QUEUE_HEADER_H, COL_TRACK);
  tft.setTextColor(TFT_WHITE, COL_TRACK);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("< Back", 12, QUEUE_HEADER_H / 2, 2);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Queue", 240, QUEUE_HEADER_H / 2, 2);
  tft.setTextDatum(TL_DATUM);

  drawQueueRows();
  drawQueuePager();
}

// Repaints the whole now-playing screen from currently-held state (cached
// art, lastTitle/lastArtist, isPlaying, position/duration, like state) —
// used to come back to it after the queue screen, which fully overwrites it.
void renderNowPlayingScreen() {
  drawStaticUI(false);
  redrawArt();
  lastElapsedLabel = "";
  lastDurationLabel = "";
  updateProgressBar();
  updateTimeLabels();
  renderTitleBox();
  renderArtistBox();
}

void openQueueScreen() {
  currentScreen = SCREEN_QUEUE;
  // Opening happens on the SAME touch whose release is then evaluated below —
  // clear any stale row from a previous queue session so that release can't be
  // read as a row-select and immediately close the screen we just opened.
  queueLongPressRow = -1;
  queueRemoveFired = false;
  fetchQueue();

  // Center the currently-playing track in view instead of always opening at
  // the top — for a long queue the selected song is often well past row 0.
  int maxOffset = queueItemCount > QUEUE_VISIBLE_ROWS ? queueItemCount - QUEUE_VISIBLE_ROWS : 0;
  queueScrollOffset = queueSelectedIndex >= 0 ? queueSelectedIndex - QUEUE_VISIBLE_ROWS / 2 : 0;
  if (queueScrollOffset < 0) queueScrollOffset = 0;
  if (queueScrollOffset > maxOffset) queueScrollOffset = maxOffset;

  drawQueueScreen();
}

void closeQueueScreen() {
  currentScreen = SCREEN_PLAYER;
  renderNowPlayingScreen();
}

// Drops the row locally (queueDoc is already fully loaded) instead of
// re-fetching the whole list — cheap and instant, no round trip to wait on.
void removeQueueItem(int idx) {
  char path[32];
  snprintf(path, sizeof(path), "/api/v1/queue/%d", idx);
  HTTPClient http;
  httpBegin(http, path);
  http.sendRequest("DELETE");
  http.end();

  queueDoc["items"].as<JsonArray>().remove(idx);
  queueItemCount--;
  if (queueSelectedIndex == idx) queueSelectedIndex = -1;
  else if (queueSelectedIndex > idx) queueSelectedIndex--;

  int maxOffset = queueItemCount > QUEUE_VISIBLE_ROWS ? queueItemCount - QUEUE_VISIBLE_ROWS : 0;
  if (queueScrollOffset > maxOffset) queueScrollOffset = maxOffset;

  drawQueueRows();
  drawQueuePager();
}

// Header and pager fire immediately on press, same as always. A row press is
// deferred instead — handleTouch() decides whether it becomes a long-press
// remove or a plain-tap jump once it knows how long the finger was held.
void handleQueueTouchDown(uint16_t x, uint16_t y) {
  queueLongPressRow = -1;
  queueRemoveFired = false;

  if (y < QUEUE_HEADER_H) {
    if (x < 90) closeQueueScreen();
    return;
  }

  int listW = 480 - QUEUE_PAGER_W;
  if (x >= listW) {
    int totalH = 320 - QUEUE_HEADER_H;
    int halfH = totalH / 2;
    if (y < QUEUE_HEADER_H + halfH) {
      if (queueScrollOffset > 0) {
        queueScrollOffset -= QUEUE_VISIBLE_ROWS;
        if (queueScrollOffset < 0) queueScrollOffset = 0;
        drawQueueRows();
        drawQueuePager();
      }
    } else if (queueScrollOffset + QUEUE_VISIBLE_ROWS < queueItemCount) {
      queueScrollOffset += QUEUE_VISIBLE_ROWS;
      drawQueueRows();
      drawQueuePager();
    }
    return;
  }

  int idx = queueScrollOffset + (y - QUEUE_HEADER_H) / QUEUE_ROW_H;
  if (idx >= 0 && idx < queueItemCount) queueLongPressRow = idx;
}

// Single touch point, sampled every loop() — press/hold/drag/release are
// derived here since XPT2046 gives no gesture events of its own.
void handleTouch() {
  uint16_t x, y;
  bool touching = tft.getTouch(&x, &y);

  if (touching && !touchWasDown) {
    touchStartX = x;
    touchStartY = y;
    touchStartMs = millis();
    touchIsDragVolume = false;
    touchLikeFired = false;
    touchBarSeekFired = false;
    dragStartVolume = currentVolume;

    if (currentScreen == SCREEN_QUEUE) {
      handleQueueTouchDown(x, y);
      touchWasDown = touching;
      return;
    }

    // Tap-to-seek fires once on press, no scrubbing — bar is a thin target
    // so a press/drag/release distinction isn't worth the complexity here.
    if (y >= BAR_Y - 12 && y <= BAR_Y + 12 && x >= BAR_X && x <= (BAR_X + BAR_W) && currentDuration > 0) {
      float frac = (float)(x - BAR_X) / (float)BAR_W;
      if (frac < 0) frac = 0;
      if (frac > 1) frac = 1;
      float seconds = frac * currentDuration;
      postActionJson("/api/v1/seek-to", "{\"seconds\":" + String(seconds, 2) + "}");
      currentPosition = seconds;
      updateProgressBar();
      updateTimeLabels();
      touchBarSeekFired = true;
    } else if (x >= QUEUE_TAB_X && x <= QUEUE_TAB_X + QUEUE_TAB_W && y >= QUEUE_TAB_Y && y <= QUEUE_TAB_Y + QUEUE_TAB_H) {
      openQueueScreen();
      touchWasDown = touching;
      return;
    } else if (x >= SHUFFLE_TAB_X && x <= SHUFFLE_TAB_X + SHUFFLE_TAB_W && y >= MODE_TAB_Y && y <= MODE_TAB_Y + MODE_TAB_H) {
      flashPress(PB_SHUFFLE);
      postAction("/api/v1/shuffle");
    } else if (x >= REPEAT_TAB_X && x <= REPEAT_TAB_X + REPEAT_TAB_W && y >= MODE_TAB_Y && y <= MODE_TAB_Y + MODE_TAB_H) {
      flashPress(PB_REPEAT);
      postActionJson("/api/v1/switch-repeat", "{\"iteration\":1}");
    }
  }

  if (currentScreen == SCREEN_PLAYER && touching && !touchBarSeekFired) {
    bool inArtZone = touchStartX >= ART_X && touchStartX <= (ART_X + ART_SIZE) && touchStartY >= ART_Y &&
                      touchStartY <= (ART_Y + ART_SIZE);

    if (inArtZone) {
      int dy = (int)touchStartY - (int)y;
      if (!touchIsDragVolume && abs(dy) > VOLUME_DRAG_THRESHOLD_PX) touchIsDragVolume = true;

      if (touchIsDragVolume) {
        int newVolume = dragStartVolume + (dy * 100) / VOLUME_DRAG_RANGE_PX;
        if (newVolume < 0) newVolume = 0;
        if (newVolume > 100) newVolume = 100;
        if (newVolume != lastSentVolume && millis() - lastVolumePostMs > VOLUME_POST_INTERVAL_MS) {
          postActionJson("/api/v1/volume", "{\"volume\":" + String(newVolume) + "}");
          lastSentVolume = newVolume;
          lastVolumePostMs = millis();
          currentVolume = newVolume;
          drawVolumeOverlay(newVolume);
        }
      } else if (!touchLikeFired && millis() - touchStartMs > LONG_PRESS_MS) {
        postAction("/api/v1/like");
        // Server toggles LIKE<->INDIFFERENT on repeated /like calls — mirror
        // that locally instead of always forcing LIKE.
        likeState = (likeState == "LIKE") ? "" : "LIKE";
        redrawArt();
        touchLikeFired = true;
      }
    } else if (y >= CTRL_Y && y <= CTRL_Y + CTRL_H && x >= ZONE_X && x <= ZONE_X + ZONE_W) {
      if (millis() - lastTouchMs >= 300) {
        lastTouchMs = millis();
        int third = ZONE_W / 3;
        if (x < ZONE_X + third) {
          flashPress(PB_PREV);
          postAction("/api/v1/previous");
        } else if (x < ZONE_X + 2 * third) {
          flashPress(PB_PLAY);
          postAction("/api/v1/toggle-play");
        } else {
          flashPress(PB_NEXT);
          postAction("/api/v1/next");
        }
      }
    }
  }

  // Same long-press idea as the art zone's like: held past the threshold
  // removes the row; released before that, it's a plain tap (see below).
  if (currentScreen == SCREEN_QUEUE && touching && queueLongPressRow >= 0 && !queueRemoveFired &&
      millis() - touchStartMs > LONG_PRESS_MS) {
    removeQueueItem(queueLongPressRow);
    queueRemoveFired = true;
  }

  if (!touching && touchWasDown) {
    if (touchIsDragVolume) clearVolumeOverlay();
    if (currentScreen == SCREEN_QUEUE && queueLongPressRow >= 0 && !queueRemoveFired) {
      patchActionJson("/api/v1/queue", "{\"index\":" + String(queueLongPressRow) + "}");
      closeQueueScreen();
    }
  }

  touchWasDown = touching;
}

void applySong(JsonVariant song) {
  if (song.isNull()) return;

  String title = song["title"] | "";
  String artist = song["artist"] | "";
  String videoId = song["videoId"] | "";
  currentDuration = song["songDuration"] | 0.0f;
  // VIDEO_CHANGED carries no top-level isPlaying (unlike PLAYER_INFO/
  // PLAYER_STATE_CHANGED) — but switching tracks while paused resumes
  // playback, and song.isPaused here is what actually reflects that. Without
  // this the play/pause icon stayed stuck on "paused" after any track change.
  isPlaying = !(song["isPaused"] | !isPlaying);

  if (title != lastTitle) setTitleText(title);
  if (artist != lastArtist) setArtistText(artist);

  if (videoId.length() > 0 && videoId != currentVideoId) {
    currentVideoId = videoId;
    // Show a cached frame right away instead of the placeholder while the
    // fresh websocket push (which always follows) is still in flight.
    if (loadArtFromCache(videoId) && currentScreen == SCREEN_PLAYER) redrawArt();
  }
}

void webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_CONNECTED || type == WStype_DISCONNECTED) {
    wsConnected = type == WStype_CONNECTED;
    if (currentScreen == SCREEN_PLAYER) redrawArt();
    return;
  }
  if (type == WStype_BIN) {
    size_t n = (length <= sizeof(lastArtJpeg)) ? length : 0;
    // Identical to what's already held/shown — e.g. applySong() just drew this
    // exact frame from the SD cache preview. Decoding it again is the redundant
    // second "scan" over pixels that already changed; skip it.
    if (n > 0 && n == lastArtJpegLen && memcmp(payload, lastArtJpeg, n) == 0) return;

    lastArtJpegLen = n;
    if (n > 0) {
      memcpy(lastArtJpeg, payload, n);
      saveArtToCache(currentVideoId, payload, length);
    }
    if (currentScreen == SCREEN_PLAYER) drawAlbumArtJpeg(payload, length);
    return;
  }
  if (type != WStype_TEXT) return;

  JsonDocument doc;
  if (deserializeJson(doc, payload, length) != DeserializationError::Ok) return;

  // const char* straight from the parsed doc instead of String copies — this
  // handler runs on every message (POSITION_CHANGED ~1/s), so building 2-3
  // throwaway Strings per message here was steady heap churn for nothing.
  const char *msgType = doc["type"] | "";
  const char *accentColor = doc["accentColor"] | "";
  if (accentColor[0] != 0) applyAccentColor(accentColor);

  int volumeField = doc["volume"] | -1;
  if (volumeField >= 0 && !touchIsDragVolume) currentVolume = volumeField;
  isMuted = doc["muted"] | isMuted;

  // Present on PLAYER_INFO (initial state) and on SHUFFLE_CHANGED/REPEAT_CHANGED
  // — absent elsewhere, in which case doc[...] just falls back to the current
  // value and modeChanged comes out false, so this is a no-op on those messages.
  bool shuffleField = doc["shuffle"] | currentShuffle;
  const char *repeatField = doc["repeat"] | currentRepeatMode.c_str();
  bool repeatChanged = strcmp(repeatField, currentRepeatMode.c_str()) != 0;
  bool modeChanged = shuffleField != currentShuffle || repeatChanged;
  currentShuffle = shuffleField;
  if (repeatChanged) currentRepeatMode = repeatField;
  if (modeChanged && currentScreen == SCREEN_PLAYER) drawModeButtons();

  if (strcmp(msgType, "PLAYER_INFO") == 0) {
    applySong(doc["song"]);
    isPlaying = doc["isPlaying"] | false;
    currentPosition = doc["position"] | 0.0f;
    if (currentScreen == SCREEN_PLAYER) drawPlayPauseIcon(!isPlaying);
    fetchLikeState();
  } else if (strcmp(msgType, "VIDEO_CHANGED") == 0) {
    applySong(doc["song"]);
    currentPosition = doc["position"] | 0.0f;
    // A stray tick from the song that just ended can arrive right after this
    // message (event-ordering race upstream) — ignore ticks for a short
    // window so they can't paint a bar/timestamp for the wrong song.
    ignorePositionUntilMs = millis() + POSITION_DEBOUNCE_MS;
    if (currentScreen == SCREEN_PLAYER) drawPlayPauseIcon(!isPlaying);
    fetchLikeState();
  } else if (strcmp(msgType, "PLAYER_STATE_CHANGED") == 0) {
    isPlaying = doc["isPlaying"] | false;
    if (millis() >= ignorePositionUntilMs) {
      currentPosition = doc["position"] | 0.0f;
    }
    if (currentScreen == SCREEN_PLAYER) drawPlayPauseIcon(!isPlaying);
  } else if (strcmp(msgType, "POSITION_CHANGED") == 0) {
    if (millis() >= ignorePositionUntilMs) {
      currentPosition = doc["position"] | 0.0f;
    }
  } else {
    return;
  }

  // State (currentPosition/currentDuration/isPlaying/...) is always kept up
  // to date above even while the queue screen is showing — only the paint is
  // skipped, since it'd otherwise draw progress-bar pixels under the list.
  if (currentScreen == SCREEN_PLAYER) {
    updateProgressBar();
    updateTimeLabels();
  }
}

void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(1);
  u8f.begin(tft);

  titleSprite.createSprite(COL_W, TEXT_BOX_H);
  artistSprite.createSprite(COL_W, TEXT_BOX_H);

  COL_BG = TFT_BLACK;
  COL_TRACK = tft.color565(55, 55, 55);
  COL_ART = tft.color565(35, 35, 35);
  COL_ICON = tft.color565(210, 210, 210);

  // Calibrate once, then reuse the saved values on every later boot instead
  // of showing "touch the corners" each time.
  prefs.begin("cyd35", false);
  size_t calLen = prefs.getBytes("touchCal", calData, sizeof(calData));
  if (calLen == sizeof(calData)) {
    tft.setTouch(calData);
  } else {
    tft.fillScreen(COL_BG);
    tft.setTextColor(TFT_WHITE, COL_BG);
    tft.setTextFont(2);
    tft.setCursor(20, 20);
    tft.println("Touch the corners");
    tft.calibrateTouch(calData, TFT_WHITE, COL_BG, 15);
    prefs.putBytes("touchCal", calData, sizeof(calData));
  }

  tft.fillScreen(COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Connecting WiFi...", 240, 150, 2);
  tft.setTextDatum(TL_DATUM);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
  }

  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tftOutput);

  // Own SPI bus (VSPI) from the TFT's — safe to init independently. No card
  // or a bad one just disables the art cache, everything else still works.
  sdAvailable = SD.begin(SD_CS_PIN);
  if (sdAvailable) {
    SD.mkdir("/art");
  } else {
    Serial.println("SD init failed — art cache disabled");
  }

  drawStaticUI();

  webSocket.begin(API_HOST, API_PORT, "/api/v1/ws");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(3000);
}

void loop() {
  webSocket.loop();
  handleTouch();
  if (flashBtn != PB_NONE && millis() >= flashClearMs) clearPressFlash();
  if (currentScreen == SCREEN_PLAYER) updateMarquee();
}
