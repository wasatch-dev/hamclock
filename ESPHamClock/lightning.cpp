/* lightning.cpp — Blitzortung lightning overlay for HamClock via OHB
 *
 * Adds a "Lightning" toggle to the Map View menu. When enabled, fetches
 * pre-filtered strike data from the OHB backend every LIGHTNING_INTERVAL
 * seconds and renders age-coloured bolt icons on the map.
 *
 * The OHB backend (blitzortung_daemon.py + lightning/strikes) handles:
 *   - Subscribing to the Blitzortung global MQTT feed
 *   - Maintaining a rolling 10-minute global strike buffer
 *   - Filtering to the DE lat/lon + radius on request
 *   - Computing age in seconds at request time
 *
 * Response format — plain text, one strike per line:
 *   lat,lon,age_seconds
 *   51.2300,-0.4500,45
 *   34.1200,-84.3000,180
 *
 * Strikes age out naturally: OHB only returns strikes younger than
 * MAX_AGE, and HamClock replaces strikes[] entirely on each fetch.
 * No explicit age-out logic is needed on the client side.
 */

#include "HamClock.h"

// ---- tunables ------------------------------------------------------------

#define LIGHTNING_INTERVAL      (5*60)  // fetch interval, seconds
#define LIGHTNING_RETRY_SECS    60      // retry after failed fetch
#define LIGHTNING_RADIUS_KM     500     // search radius passed to OHB
#define LIGHTNING_MAX_STRIKES   200     // heap guard for ESP8266

static const char ltg_strikes[] = "/ham/HamClock/lightning/strikes.pl";

// ---- types ---------------------------------------------------------------

typedef struct {
    float lat;      // decimal degrees N
    float lng;      // decimal degrees E
    int   age_s;    // seconds old at time of last fetch
} LightningStrike;

// ---- module state --------------------------------------------------------

static LightningStrike  strikes[LIGHTNING_MAX_STRIKES];
static int              n_strikes;
static time_t           next_fetch;

uint8_t lightning_on;   // extern; saved to NV_LIGHTNING_ON

// ---- bolt icon -----------------------------------------------------------
//
// 13-pixel-tall ⚡ shape:
//   cx+2,cy-6  to  cx-2,cy-1   top stroke
//   cx-4,cy    to  cx+4,cy     horizontal bar
//   cx+2,cy+1  to  cx-2,cy+6   bottom stroke
//   white centre pixel for visibility on any background

static void drawBolt (int16_t cx, int16_t cy, uint16_t color)
{
    tft.drawLineRaw (cx+2, cy-6,  cx-2, cy-1,  2, color);
    tft.drawLineRaw (cx-4, cy,    cx+4, cy,     2, color);
    tft.drawLineRaw (cx+2, cy+1,  cx-2, cy+6,  2, color);
    tft.drawPixelRaw (cx, cy, RA8875_WHITE);
}

// ---- response parsing ----------------------------------------------------
//
// OHB returns plain text, one strike per line:
//   lat,lon,age_seconds
//
// All filtering and age calculation is done server-side.

static int parseStrikes (const char *buf, int buf_len)
{
    int count = 0;
    const char *p   = buf;
    const char *end = buf + buf_len;

    while (p < end && count < LIGHTNING_MAX_STRIKES) {
        float lat, lon;
        int   age_s;

        if (sscanf (p, "%f,%f,%d", &lat, &lon, &age_s) == 3 && age_s >= 0) {
            strikes[count].lat   = lat;
            strikes[count].lng   = lon;
            strikes[count].age_s = age_s;
            count++;
        }

        const char *nl = (const char *) memchr (p, '\n', end - p);
        if (!nl) break;
        p = nl + 1;
    }

    return count;
}

// ---- fetch ---------------------------------------------------------------

static bool fetchLightning (void)
{
    WiFiClient client;
    bool ok = false;

    if (!client.connect (backend_host, backend_port)) {
        Serial.printf ("LTG: connect %s:%d failed\n", backend_host, backend_port);
        return false;
    }

    // Build and send plain HTTP GET directly — httpHCGET is designed for
    // the HamClock backend's custom protocol and doesn't work correctly
    // with standard CGI responses from lighttpd.
    client.print ("GET /ham/HamClock/lightning/strikes.pl?lat=");
    client.print (de_ll.lat_d, 4);
    client.print ("&lon=");
    client.print (de_ll.lng_d, 4);
    client.print ("&radius=");
    client.print (LIGHTNING_RADIUS_KM);
    client.print (" HTTP/1.0\r\n");
    client.print ("Host: ");
    client.println (backend_host);
    client.print ("Connection: close\r\n\r\n");

    if (!httpSkipHeader (client)) {
        Serial.printf ("LTG: no HTTP header\n");
        goto out;
    }

    {
        const int BUFSZ = 16384;
        char *buf = (char *) malloc (BUFSZ);
        if (!buf) {
            Serial.printf ("LTG: malloc %d failed\n", BUFSZ);
            goto out;
        }

        int  pos = 0;
        char line[128];
        while (getTCPLine (client, line, sizeof(line), NULL)) {
            int len = strlen (line);
            if (pos + len + 1 < BUFSZ) {
                memcpy (buf + pos, line, len);
                buf[pos + len] = '\n';
                pos += len + 1;
            }
        }
        buf[pos] = '\0';

        n_strikes = parseStrikes (buf, pos);
        free (buf);

        Serial.printf ("LTG: %d strikes within %dkm\n",
                       n_strikes, LIGHTNING_RADIUS_KM);
        ok = true;     // empty response is valid — no storms nearby
    }

out:
    client.stop();
    return ok;
}

// ---- public API ----------------------------------------------------------

/* Clear strikes and reset fetch timer.
 * Single function covers both toggle-on (immediate refetch) and
 * toggle-off (nothing to draw until re-enabled).
 */
void resetLightning (void)
{
    n_strikes  = 0;
    next_fetch = 0;
}

/* Restore NV state at startup. */
void initLightning (void)
{
    if (!NVReadUInt8 (NV_LIGHTNING_ON, &lightning_on))
        lightning_on = 0;
    n_strikes  = 0;
    next_fetch = 0;
    Serial.printf ("LTG: init, overlay %s\n", lightning_on ? "ON" : "OFF");
}

/* Fetch fresh data if interval elapsed. Called from updateWiFi(). */
void updateLightning (void)
{
    if (!lightning_on)
        return;

    time_t now = myNow();
    if (now < next_fetch)
        return;

    if (fetchLightning())
        next_fetch = now + LIGHTNING_INTERVAL;
    else
        next_fetch = now + LIGHTNING_RETRY_SECS;
}

/* Draw current strikes on the map. Called from drawAllSymbols().
 *
 * Colour encodes age at time of last fetch:
 *   < 2 min  — bright yellow
 *   < 5 min  — orange
 *   older    — red
 *
 * Also draws a "Blitzortung.org" attribution over Antarctica, centred
 * on the map, matching Blitzortung's data usage policy requirement.
 */
void drawLightningOnMap (void)
{
    if (!lightning_on || n_strikes == 0)
        return;

    for (int i = 0; i < n_strikes; i++) {

        SCoord s;
        ll2sRaw (strikes[i].lat * (M_PIF / 180.0F),
                 strikes[i].lng * (M_PIF / 180.0F),
                 s, 4);

        if (s.x == 0 && s.y == 0)
            continue;

        uint16_t color;
        if      (strikes[i].age_s < 120)  color = RGB565(255, 220,   0);
        else if (strikes[i].age_s < 300)  color = RGB565(255, 140,   0);
        else                              color = RGB565(220,  40,  40);

        drawBolt ((int16_t)s.x, (int16_t)s.y, color);
    }

    // Attribution label centred over Antarctica (bottom of Mercator map)
    static const char credit[] = "Blitzortung.org";
    selectFontStyle (LIGHT_FONT, FAST_FONT);
    uint16_t cw = getTextWidth (credit);
    tft.setCursor (map_b.x + (map_b.w - cw)/2,
                   map_b.y + map_b.h - 10);
    tft.setTextColor (RGB565(180, 180, 180));   // subtle grey
    tft.print (credit);
}
