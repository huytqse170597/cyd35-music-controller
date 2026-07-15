#include <SPI.h>
#include <TFT_eSPI.h>
#include <U8g2_for_TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TJpg_Decoder.h>
#include <WebSocketsClient.h>
#include <Preferences.h>
#include "secrets.h" // WIFI_SSID, WIFI_PASS, API_HOST, API_PORT, API_BASE — see secrets.h.example

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

// Art arrives pre-scaled to ART_SIZE and already re-encoded as a small JPEG
// by the desktop app's api-server plugin (see websocket.ts broadcastAlbumArt) —
// this device only decodes, it never fetches or resizes the original thumbnail.
void drawAlbumArtJpeg(uint8_t *payload, size_t length) {
  if (length == 0) {
    drawArtPlaceholder();
    return;
  }

  uint16_t jw = 0, jh = 0;
  TJpgDec.getJpgSize(&jw, &jh, payload, length);
  tft.fillRect(ART_X, ART_Y, ART_SIZE, ART_SIZE, COL_BG);
  int ox = ART_X + (ART_SIZE - (int)jw) / 2;
  int oy = ART_Y + (ART_SIZE - (int)jh) / 2;
  TJpgDec.drawJpg(ox, oy, payload, length);
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

void drawStaticUI() {
  tft.resetViewport();
  tft.fillScreen(COL_BG);
  drawArtPlaceholder();
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

// Draws the title/artist box relative to its own viewport: setViewport()
// clips (and shifts the coordinate origin) so scrolled/oversized text never
// paints outside the box — this is what used to leave stray pixels behind
// once a shorter string replaced a longer one.
//
// Drawn into an off-screen sprite first, then blitted in one pushSprite()
// call — drawing the clear+text directly to the panel every scroll tick
// was visible as flicker (each glyph is its own small SPI write, so the
// panel briefly shows the cleared background between them).
void renderTextBox(TFT_eSprite &sprite, int boxX, int boxY, const String &text, int textWidth, int scrollX, uint16_t color) {
  sprite.fillSprite(COL_BG);
  u8f.begin(sprite);
  u8f.setForegroundColor(color);
  u8f.setFont(u8g2_font_unifont_t_vietnamese2);

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

void renderTitleBox() { renderTextBox(titleSprite, COL_X, TITLE_BOX_Y, lastTitle, titleWidthPx, titleScrollX, TFT_WHITE); }
void renderArtistBox() { renderTextBox(artistSprite, COL_X, ARTIST_BOX_Y, lastArtist, artistWidthPx, artistScrollX, TFT_LIGHTGREY); }

void setTitleText(const String &text) {
  lastTitle = text;
  u8f.setFont(u8g2_font_unifont_t_vietnamese2);
  titleWidthPx = u8f.getUTF8Width(text.c_str());
  titleScrollX = 0;
  renderTitleBox();
}

void setArtistText(const String &text) {
  lastArtist = text;
  u8f.setFont(u8g2_font_unifont_t_vietnamese2);
  artistWidthPx = u8f.getUTF8Width(text.c_str());
  artistScrollX = 0;
  renderArtistBox();
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
uint16_t rgb565FromHexScaled(const String &hex, float scale) {
  if (hex.length() < 7 || hex[0] != '#') return tft.color565(20, 20, 20);
  long val = strtol(hex.substring(1).c_str(), nullptr, 16);
  int r = (int)(((val >> 16) & 0xFF) * scale);
  int g = (int)(((val >> 8) & 0xFF) * scale);
  int b = (int)((val & 0xFF) * scale);
  if (r > 255) r = 255;
  if (g > 255) g = 255;
  if (b > 255) b = 255;
  return tft.color565(r, g, b);
}

void applyAccentColor(const String &hex) {
  if (hex.length() == 0) return;
  COL_BG = rgb565FromHexScaled(hex, 0.28f);
  COL_ART = rgb565FromHexScaled(hex, 0.45f);
  COL_TRACK = rgb565FromHexScaled(hex, 0.55f);

  drawStaticUI();
  renderTitleBox();
  renderArtistBox();

  lastElapsedLabel = "\x01";
  lastDurationLabel = "\x01";
  updateProgressBar();
  updateTimeLabels();
}

void postAction(const char *path) {
  HTTPClient http;
  http.begin(String(API_BASE) + path);
  http.POST("");
  http.end();
}

void handleTouch() {
  uint16_t x, y;
  if (!tft.getTouch(&x, &y)) return;
  if (millis() - lastTouchMs < 300) return;
  lastTouchMs = millis();

  if (y < CTRL_Y || y > CTRL_Y + CTRL_H) return;
  if (x < ZONE_X || x > ZONE_X + ZONE_W) return;

  int third = ZONE_W / 3;
  if (x < ZONE_X + third) {
    postAction("/api/v1/previous");
  } else if (x < ZONE_X + 2 * third) {
    postAction("/api/v1/toggle-play");
  } else {
    postAction("/api/v1/next");
  }
}

void applySong(JsonVariant song) {
  if (song.isNull()) return;

  String title = song["title"] | "";
  String artist = song["artist"] | "";
  currentDuration = song["songDuration"] | 0.0f;

  if (title != lastTitle) setTitleText(title);
  if (artist != lastArtist) setArtistText(artist);
}

void webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_BIN) {
    drawAlbumArtJpeg(payload, length);
    return;
  }
  if (type != WStype_TEXT) return;

  JsonDocument doc;
  if (deserializeJson(doc, payload, length) != DeserializationError::Ok) return;

  String msgType = doc["type"] | "";
  String accentColor = doc["accentColor"] | "";
  if (accentColor.length() > 0) applyAccentColor(accentColor);

  if (msgType == "PLAYER_INFO") {
    applySong(doc["song"]);
    isPlaying = doc["isPlaying"] | false;
    currentPosition = doc["position"] | 0.0f;
    drawPlayPauseIcon(!isPlaying);
  } else if (msgType == "VIDEO_CHANGED") {
    applySong(doc["song"]);
    currentPosition = doc["position"] | 0.0f;
    // A stray tick from the song that just ended can arrive right after this
    // message (event-ordering race upstream) — ignore ticks for a short
    // window so they can't paint a bar/timestamp for the wrong song.
    ignorePositionUntilMs = millis() + POSITION_DEBOUNCE_MS;
  } else if (msgType == "PLAYER_STATE_CHANGED") {
    isPlaying = doc["isPlaying"] | false;
    if (millis() >= ignorePositionUntilMs) {
      currentPosition = doc["position"] | 0.0f;
    }
    drawPlayPauseIcon(!isPlaying);
  } else if (msgType == "POSITION_CHANGED") {
    if (millis() >= ignorePositionUntilMs) {
      currentPosition = doc["position"] | 0.0f;
    }
  } else {
    return;
  }

  updateProgressBar();
  updateTimeLabels();
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

  drawStaticUI();

  webSocket.begin(API_HOST, API_PORT, "/api/v1/ws");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(3000);
}

void loop() {
  webSocket.loop();
  handleTouch();
  updateMarquee();
}
