#include "alarm.h"
#include "button_handler.h"
#include "devices.h"
#include "rtc_clock.h"
#include "storage.h"

#include <dk_buttons_and_leds.h>
#include <zephyr/device.h>
#include <zephyr/display/cfb.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include <cstdio>
#include <cstring>

// Typsnittstabellen (Latin-1, tecken 32-255) genereras av tools/gen_font.ps1.
#include "font_unscii_8x16.inc"

FONT_ENTRY_DEFINE(font_unscii, 8, 16, CFB_FONT_MONO_VPACKED, font_unscii_8x16, 32, 255);

namespace {

constexpr uint8_t kButtonUp = 0x01U;    // SW1
constexpr uint8_t kButtonNext = 0x02U;  // SW2
constexpr uint8_t kButtonDown = 0x04U;  // SW3
constexpr uint8_t kButtonMode = 0x08U;  // SW4

constexpr int64_t kHoldRepeatMs = 1000;

constexpr int kBigDigitWidth = 15;
constexpr int kBigColonWidth = 4;
constexpr int kBigGap = 3;
constexpr int kBigClockWidth = 6 * kBigDigitWidth + 2 * kBigColonWidth + 7 * kBigGap;

constexpr uint32_t kEventButton = BIT(0);
constexpr int64_t kSetupTimeoutMs = 10000;
constexpr int64_t kBlinkHalfPeriodMs = 500;
constexpr uint32_t kSecondsPerDay = 86400U;

enum class Field : uint8_t { Hours, Minutes, Seconds, Year, Month, Day };

constexpr uint8_t kFieldCount = 6;
constexpr uint8_t kModeSetup = 99;
constexpr uint8_t kModeAlarm = 50;
constexpr uint8_t kModeSensors = 4; // cyklar tid -> temperatur -> luftfuktighet
constexpr uint8_t kModePair = 60;   // mata in parningskod
constexpr uint8_t kModeList = 61;   // lista över enheter och koder

constexpr int64_t kSensorViewMs = 2000;
constexpr int64_t kPairMessageMs = 1500;
// Fält i parningsläget: 0-10 = siffra 1-11, 11 = plats, 12 = spara.
constexpr uint8_t kPairFieldSlot = kPairingCodeDigits;
constexpr uint8_t kPairFieldSave = kPairingCodeDigits + 1;
constexpr uint8_t kPairFieldCount = kPairingCodeDigits + 2;

// Fält i alarmläget: 0 = alarmnummer, 1 = timme, 2 = minut, 3-9 = måndag-söndag, 10 = på/av.
constexpr uint8_t kAlarmFieldCount = 11;
constexpr uint8_t kAlarmFieldHour = 1;
constexpr uint8_t kAlarmFieldMinute = 2;
constexpr uint8_t kAlarmFieldFirstDay = 3;
constexpr uint8_t kAlarmFieldEnabled = 10;

constexpr int64_t kAlarmMaxMs = 60000;
constexpr int64_t kAlarmLedHalfPeriodMs = 100;  // 5 Hz
constexpr int64_t kAlarmDisplayHalfPeriodMs = 250; // 2 Hz

const struct device *const kDisplay = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

K_EVENT_DEFINE(g_events);
atomic_t g_pressed;

bool g_display_ready;
uint8_t g_font_width = 10;
uint8_t g_font_height = 16;

uint8_t g_mode; // 0 = klocka, 1 = klocka + veckodag, 2 = datum, 3 = tid + datum, 50 = alarm, 99 = ställa
uint8_t g_default_mode; // visningsläget som återställs efter timeout i ställ-lägena
Field g_field = Field::Hours;
uint8_t g_alarm_index;
uint8_t g_alarm_field;
bool g_alarm_dirty;
bool g_inverted;

uint8_t g_pair_slot;
uint8_t g_pair_field;
char g_pair_code[kPairingCodeDigits + 1];
const char *g_pair_message;
int64_t g_pair_message_until;
uint8_t g_list_index;

bool InEditMode()
{
	return g_mode == kModeSetup || g_mode == kModeAlarm || g_mode == kModePair ||
	       g_mode == kModeList;
}

// Börjar om med tom kod; föreslår första lediga plats (annars plats 1).
void ResetPairEditor()
{
	memset(g_pair_code, '0', kPairingCodeDigits);
	g_pair_code[kPairingCodeDigits] = '\0';
	g_pair_field = 0;
	g_pair_slot = 0;
	for (int i = 0; i < kDeviceCount; i++) {
		if (DeviceCode(i)[0] == '\0') {
			g_pair_slot = static_cast<uint8_t>(i);
			break;
		}
	}
}

// Formaterar 11 siffror som XXXX-XXX-XXXX.
void FormatCode(const char *code, char *out)
{
	snprintf(out, 14, "%.4s-%.3s-%.4s", code, code + 4, code + 7);
}

// Upp/ned som hålls inne repeterar bara för tal (tid), inte för av/på-val.
bool HoldRepeats()
{
	return g_mode == kModeSetup ||
	       (g_mode == kModeAlarm &&
		(g_alarm_field == kAlarmFieldHour || g_alarm_field == kAlarmFieldMinute));
}

const char kAlarmDayLetters[8] = "MTOTFLS";

const char *FieldName(Field field)
{
	switch (field) {
	case Field::Hours:
		return "hours";
	case Field::Minutes:
		return "minutes";
	case Field::Seconds:
		return "seconds";
	case Field::Year:
		return "year";
	case Field::Month:
		return "month";
	default:
		return "day";
	}
}

// Veckodag 0 = måndag. Unix-tid 0 (1970-01-01) var en torsdag.
unsigned int Weekday(uint32_t unix_time)
{
	return (unix_time / kSecondsPerDay + 3U) % 7U;
}

const char *const kWeekdayProtocol[7] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};

// Latin-1: å = \345, ö = \366.
const char *const kWeekdayText[7] = {"M\345", "Ti", "On", "To", "Fr", "L\366", "S\366"};

void InitDisplay()
{
	if (!device_is_ready(kDisplay)) {
		printk("ERROR display=not_ready\r\n");
		return;
	}

	const int err = cfb_framebuffer_init(kDisplay);
	if (err) {
		printk("ERROR display=cfb_init code=%d\r\n", err);
		return;
	}

	// Eget Latin-1-typsnitt (8x16, med åäö); standardtypsnitten är bara ASCII.
	int font_index = 0;

	for (int i = 0; i < cfb_get_numof_fonts(kDisplay); i++) {
		uint8_t w;
		uint8_t h;

		cfb_get_font_size(kDisplay, i, &w, &h);
		if (w == 8 && h == 16) {
			font_index = i;
		}
	}

	cfb_get_font_size(kDisplay, font_index, &g_font_width, &g_font_height);
	cfb_framebuffer_set_font(kDisplay, font_index);
	g_display_ready = true;
	printk("DISPLAY ok driver=ssd1306 size=128x32 addr=0x3c font=%ux%u\r\n",
	       static_cast<unsigned int>(g_font_width), static_cast<unsigned int>(g_font_height));
}

// cfb_framebuffer_invert() växlar bara en flagga som används vid nästa finalize, så vi håller reda på läget.
void SetInverted(bool inverted)
{
	if (inverted != g_inverted) {
		cfb_framebuffer_invert(kDisplay);
		g_inverted = inverted;
	}
}

// Returnerar true om innehållet skiljer sig från senast ritade bilden (och minns det nya).
// Nya bilder ritas som standard utan inverterad färg.
bool FrameChanged(const char *key)
{
	static char last[48];

	if (!g_display_ready || !strcmp(key, last)) {
		return false;
	}

	snprintf(last, sizeof(last), "%s", key);
	SetInverted(false);
	return true;
}

void DrawGlyph(int x, char key);

// Ritar två textrader centrerade; hoppar över uppdateringen om innehållet är oförändrat.
// Strängarna är Latin-1 (t.ex. "P\305" = PÅ).
void DrawLines(const char *line1, const char *line2)
{
	char key[48];

	snprintf(key, sizeof(key), "L|%s|%s", line1, line2);
	if (!FrameChanged(key)) {
		return;
	}

	cfb_framebuffer_clear(kDisplay, false);
	cfb_print(kDisplay, line1, (128 - static_cast<int>(strlen(line1)) * g_font_width) / 2, 0);
	cfb_print(kDisplay, line2, (128 - static_cast<int>(strlen(line2)) * g_font_width) / 2,
		  g_font_height);
	cfb_framebuffer_finalize(kDisplay);
}

void FillRect(int x, int y, int w, int h)
{
	for (int row = 0; row < h; row++) {
		const struct cfb_position start = {static_cast<int16_t>(x), static_cast<int16_t>(y + row)};
		const struct cfb_position end = {static_cast<int16_t>(x + w - 1),
						 static_cast<int16_t>(y + row)};

		cfb_draw_line(kDisplay, &start, &end);
	}
}

// Sjusegmentssiffra i full displayhöjd (32 px). Bit 0-6 = segment a-g.
void DrawBigDigit(int x, int digit)
{
	constexpr int kW = kBigDigitWidth;
	constexpr int kH = 32;
	constexpr int kT = 3;
	constexpr int kHalf = (kH - kT) / 2 - 1;
	static const uint8_t kSegments[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66,
					      0x6D, 0x7D, 0x07, 0x7F, 0x6F};
	const uint8_t seg = kSegments[digit];

	if (seg & 0x01) FillRect(x + 1, 0, kW - 2, kT);                              // a
	if (seg & 0x02) FillRect(x + kW - kT, 1, kT, kHalf);                         // b
	if (seg & 0x04) FillRect(x + kW - kT, kH / 2, kT, kHalf);                    // c
	if (seg & 0x08) FillRect(x + 1, kH - kT, kW - 2, kT);                        // d
	if (seg & 0x10) FillRect(x, kH / 2, kT, kHalf);                              // e
	if (seg & 0x20) FillRect(x, 1, kT, kHalf);                                   // f
	if (seg & 0x40) FillRect(x + 1, (kH - kT) / 2, kW - 2, kT);                  // g
}

// 5x8-bitmappar (bit 4 = vänster kolumn) för veckodagarnas bokstäver, ritade i skala 4 = 20x32 px.
struct Glyph {
	char key;
	uint8_t rows[8];
};

const Glyph kGlyphs[] = {
	{'M', {0x00, 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
	{'a', {0x04, 0x0A, 0x04, 0x0E, 0x01, 0x0F, 0x11, 0x0F}}, // å
	{'T', {0x00, 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
	{'i', {0x00, 0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E}},
	{'O', {0x00, 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
	{'n', {0x00, 0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11}},
	{'o', {0x00, 0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E}},
	{'F', {0x00, 0x1F, 0x10, 0x1E, 0x10, 0x10, 0x10, 0x10}},
	{'r', {0x00, 0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10}},
	{'L', {0x00, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
	{'u', {0x00, 0x0A, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E}}, // ö
	{'S', {0x00, 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
	{'A', {0x00, 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
	{'R', {0x00, 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
};

// Veckodagarnas bokstäver; 'a' = å och 'u' = ö i glyftabellen ovan.
const char *const kWeekdayGlyphs[7] = {"Ma", "Ti", "On", "To", "Fr", "Lu", "Su"};

constexpr int kGlyphScale = 4;
constexpr int kGlyphWidth = 5 * kGlyphScale;
constexpr int kLetterGap = 3;
constexpr int kWeekdayWidth = 2 * kGlyphWidth + kLetterGap;

void DrawGlyph(int x, char key)
{
	for (const Glyph &glyph : kGlyphs) {
		if (glyph.key != key) {
			continue;
		}

		for (int row = 0; row < 8; row++) {
			int col = 0;

			while (col < 5) {
				if (!(glyph.rows[row] & (0x10U >> col))) {
					col++;
					continue;
				}

				int run = 0;

				while (col + run < 5 && (glyph.rows[row] & (0x10U >> (col + run)))) {
					run++;
				}

				FillRect(x + col * kGlyphScale, row * kGlyphScale, run * kGlyphScale,
					 kGlyphScale);
				col += run;
			}
		}
		return;
	}
}

void DrawBigDigitPair(int *x, unsigned int value)
{
	DrawBigDigit(*x, static_cast<int>(value / 10U));
	*x += kBigDigitWidth + kBigGap;
	DrawBigDigit(*x, static_cast<int>(value % 10U));
	*x += kBigDigitWidth;
}

void DrawBigColon(int *x)
{
	FillRect(*x, 9, kBigColonWidth, kBigColonWidth);
	FillRect(*x, 21, kBigColonWidth, kBigColonWidth);
	*x += kBigColonWidth;
}

// Mode 0: HH:mm:ss. Mode 1 (with_weekday): HH:mm och två bokstäver för veckodag i stället för sekunder.
void DrawBigClock(uint32_t unix_time, bool with_weekday)
{
	const uint32_t tod = unix_time % kSecondsPerDay;
	const unsigned int hours = static_cast<unsigned int>(tod / 3600U);
	const unsigned int minutes = static_cast<unsigned int>((tod / 60U) % 60U);
	const unsigned int seconds = static_cast<unsigned int>(tod % 60U);
	const unsigned int weekday = Weekday(unix_time);
	char key[16];

	if (with_weekday) {
		snprintf(key, sizeof(key), "W|%02u%02u%u", hours, minutes, weekday);
	} else {
		snprintf(key, sizeof(key), "B|%02u%02u%02u", hours, minutes, seconds);
	}
	if (!FrameChanged(key)) {
		return;
	}

	cfb_framebuffer_clear(kDisplay, false);

	const int hm_width = 4 * kBigDigitWidth + kBigColonWidth + 4 * kBigGap;
	const int total = with_weekday ? hm_width + kBigGap * 2 + kWeekdayWidth + 2
				       : kBigClockWidth;
	int x = (128 - total) / 2;

	DrawBigDigitPair(&x, hours);
	x += kBigGap;
	DrawBigColon(&x);
	x += kBigGap;
	DrawBigDigitPair(&x, minutes);

	if (with_weekday) {
		x += kBigGap * 2 + 2;
		DrawGlyph(x, kWeekdayGlyphs[weekday][0]);
		DrawGlyph(x + kGlyphWidth + kLetterGap, kWeekdayGlyphs[weekday][1]);
	} else {
		x += kBigGap;
		DrawBigColon(&x);
		x += kBigGap;
		DrawBigDigitPair(&x, seconds);
	}

	cfb_framebuffer_finalize(kDisplay);
}

struct Date {
	int year;
	unsigned int month; // 1-12
	unsigned int day;   // 1-31
};

constexpr int kYearMin = 1970;
constexpr int kYearMax = 2099;

// Dagar sedan 1970-01-01 -> datum (Howard Hinnants civil_from_days).
Date DateFromDays(uint32_t days)
{
	const int64_t z = static_cast<int64_t>(days) + 719468;
	const int64_t era = z / 146097;
	const int64_t doe = z - era * 146097;
	const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const int64_t mp = (5 * doy + 2) / 153;
	const unsigned int day = static_cast<unsigned int>(doy - (153 * mp + 2) / 5 + 1);
	const unsigned int month = static_cast<unsigned int>(mp < 10 ? mp + 3 : mp - 9);

	return {static_cast<int>(yoe + era * 400 + (month <= 2 ? 1 : 0)), month, day};
}

// Datum -> dagar sedan 1970-01-01 (days_from_civil).
uint32_t DaysFromDate(const Date &date)
{
	const int y = date.year - (date.month <= 2 ? 1 : 0);
	const int era = y / 400;
	const int yoe = y - era * 400;
	const int doy = static_cast<int>((153 * (date.month > 2 ? date.month - 3 : date.month + 9) + 2) / 5 +
					 date.day - 1);
	const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

	return static_cast<uint32_t>(era * 146097 + doe - 719468);
}

unsigned int DaysInMonth(int year, unsigned int month)
{
	static const uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;

	return (month == 2 && leap) ? 29U : kDays[month - 1];
}

// Nytt värde i [lo, hi] med wrap. jump_to_tens går till nästa jämna tiotal i stället för ett steg.
int NextValue(int value, int step, bool jump_to_tens, int lo, int hi)
{
	int next;

	if (!jump_to_tens) {
		next = value + step;
	} else if (step > 0) {
		next = (value / 10 + 1) * 10;
	} else {
		next = (value % 10 == 0) ? value - 10 : (value / 10) * 10;
	}

	if (next > hi) {
		return lo;
	}
	if (next < lo) {
		return jump_to_tens ? (hi / 10) * 10 : hi;
	}
	return next;
}

// Ändrar vald tid- eller datumdel utan att påverka de andra delarna och låter den rulla runt.
// step = +1/-1 ger ett steg; jump_to_tens flyttar i stället till nästa jämna tiotal i riktningen.
void AdjustField(int step, bool jump_to_tens = false)
{
	const uint32_t old_unix = RtcClockGet();
	const uint32_t tod = old_unix % kSecondsPerDay;
	Date date = DateFromDays(old_unix / kSecondsPerDay);
	uint32_t new_tod = tod;

	switch (g_field) {
	case Field::Hours:
		new_tod = static_cast<uint32_t>(NextValue(static_cast<int>(tod / 3600U), step,
							  jump_to_tens, 0, 23)) * 3600U +
			  tod % 3600U;
		break;
	case Field::Minutes:
		new_tod = (tod / 3600U) * 3600U +
			  static_cast<uint32_t>(NextValue(static_cast<int>((tod / 60U) % 60U), step,
							  jump_to_tens, 0, 59)) * 60U +
			  tod % 60U;
		break;
	case Field::Seconds:
		new_tod = (tod / 60U) * 60U +
			  static_cast<uint32_t>(NextValue(static_cast<int>(tod % 60U), step,
							  jump_to_tens, 0, 59));
		break;
	case Field::Year:
		date.year = NextValue(date.year, step, jump_to_tens, kYearMin, kYearMax);
		break;
	case Field::Month:
		date.month = static_cast<unsigned int>(
			NextValue(static_cast<int>(date.month), step, jump_to_tens, 1, 12));
		break;
	default:
		date.day = static_cast<unsigned int>(NextValue(static_cast<int>(date.day), step,
							       jump_to_tens, 1,
							       static_cast<int>(DaysInMonth(date.year, date.month))));
		break;
	}

	// Månad/år kan göra att dagen blir för stor (31 -> februari); kapa till månadens längd.
	const unsigned int max_day = DaysInMonth(date.year, date.month);

	if (date.day > max_day) {
		date.day = max_day;
	}

	const uint32_t new_unix = DaysFromDate(date) * kSecondsPerDay + new_tod;

	// Differensen som wrap-around-aritmetik, så den även fungerar för stora hopp (> 68 år).
	RtcClockAdjust(static_cast<int32_t>(new_unix - old_unix));
}

// Sätter dubbla mellanslag på det blinkande fältet i en textsträng med fasta kolumner.
void BlankField(char *text, int offset)
{
	text[offset] = ' ';
	text[offset + 1] = ' ';
}

void FormatTime(uint32_t unix_time, bool hide_field, char *out, size_t size)
{
	const uint32_t tod = unix_time % kSecondsPerDay;
	const Date date = DateFromDays(unix_time / kSecondsPerDay);
	const bool show_date = g_field >= Field::Year;
	char text[16];

	// Rad 1 visar tiden för tidsfält och YYYY-MM-DD för datumfält.
	if (show_date) {
		snprintf(text, sizeof(text), "%04d-%02u-%02u", date.year, date.month, date.day);
	} else {
		snprintf(text, sizeof(text), "%02u:%02u:%02u", static_cast<unsigned int>(tod / 3600U),
			 static_cast<unsigned int>((tod / 60U) % 60U),
			 static_cast<unsigned int>(tod % 60U));
	}

	if (hide_field) {
		switch (g_field) {
		case Field::Hours:
			BlankField(text, 0);
			break;
		case Field::Minutes:
			BlankField(text, 3);
			break;
		case Field::Seconds:
			BlankField(text, 6);
			break;
		case Field::Year:
			BlankField(text, 0);
			BlankField(text, 2);
			break;
		case Field::Month:
			BlankField(text, 5);
			break;
		default:
			BlankField(text, 8);
			break;
		}
	}

	snprintf(out, size, "%s", text);
}

// Mode 2: YY-MM-DD i samma stora sjusegmentsstil som klockan.
void DrawBigDate(uint32_t unix_time)
{
	constexpr int kDashWidth = 6;
	constexpr int kTotal = 6 * kBigDigitWidth + 2 * kDashWidth + 7 * kBigGap;
	const Date date = DateFromDays(unix_time / kSecondsPerDay);
	const unsigned int yy = static_cast<unsigned int>(date.year % 100);
	char key[16];

	snprintf(key, sizeof(key), "D|%02u%02u%02u", yy, date.month, date.day);
	if (!FrameChanged(key)) {
		return;
	}

	cfb_framebuffer_clear(kDisplay, false);

	int x = (128 - kTotal) / 2;
	const unsigned int parts[3] = {yy, date.month, date.day};

	for (int i = 0; i < 3; i++) {
		DrawBigDigitPair(&x, parts[i]);
		if (i < 2) {
			x += kBigGap;
			FillRect(x, 14, kDashWidth, 3);
			x += kDashWidth + kBigGap;
		}
	}

	cfb_framebuffer_finalize(kDisplay);
}

// Mode 3: HH:mm:ss på första raden, YY-MM-DD och veckodag på andra.
void DrawTimeDate(uint32_t unix_time)
{
	const uint32_t tod = unix_time % kSecondsPerDay;
	const Date date = DateFromDays(unix_time / kSecondsPerDay);
	char line1[16];
	char line2[16];

	snprintf(line1, sizeof(line1), "%02u:%02u:%02u", static_cast<unsigned int>(tod / 3600U),
		 static_cast<unsigned int>((tod / 60U) % 60U), static_cast<unsigned int>(tod % 60U));
	snprintf(line2, sizeof(line2), "%02u-%02u-%02u %s", static_cast<unsigned int>(date.year % 100),
		 date.month, date.day, kWeekdayText[Weekday(unix_time)]);
	DrawLines(line1, line2);
}

void DrawAlarmFrame()
{
	if (!FrameChanged("A")) {
		return;
	}

	cfb_framebuffer_clear(kDisplay, false);

	const char *const word = "ALARM";
	int x = (128 - (5 * kGlyphWidth + 4 * kLetterGap)) / 2;

	for (int i = 0; i < 5; i++) {
		DrawGlyph(x, word[i]);
		x += kGlyphWidth + kLetterGap;
	}

	SetInverted(true);
	cfb_framebuffer_finalize(kDisplay);
}

void FormatDays(uint8_t days, char *out)
{
	for (int i = 0; i < 7; i++) {
		out[i] = (days & (1U << i)) ? kAlarmDayLetters[i] : '-';
	}
	out[7] = '\0';
}

void PrintAlarm(int index)
{
	const Alarm *alarm = AlarmAt(index);
	char days[8];

	FormatDays(alarm->days, days);
	printk("ALARM id=%d time=%02u:%02u days=%s enabled=%u\r\n", index + 1,
	       static_cast<unsigned int>(alarm->hour), static_cast<unsigned int>(alarm->minute),
	       days, static_cast<unsigned int>(alarm->enabled));
}

void PrintAllAlarms()
{
	for (int i = 0; i < kAlarmCount; i++) {
		PrintAlarm(i);
	}
}

const char *AlarmFieldName(uint8_t field)
{
	static const char *const kNames[kAlarmFieldCount] = {
		"alarm", "hours", "minutes", "mon", "tue", "wed", "thu", "fri", "sat", "sun", "enabled"};

	return kNames[field];
}

void SaveAlarmsIfDirty()
{
	if (!g_alarm_dirty) {
		return;
	}

	g_alarm_dirty = false;

	const int err = AlarmsSave();

	if (err) {
		printk("ERROR alarm_save code=%d\r\n", err);
	} else {
		printk("ALARM_SAVED\r\n");
		PrintAllAlarms();
	}
}

void AdjustAlarm(int step, bool jump_to_tens = false)
{
	Alarm *alarm = AlarmAt(g_alarm_index);

	if (g_alarm_field == 0) {
		g_alarm_index = static_cast<uint8_t>(NextValue(g_alarm_index, step, false, 0, kAlarmCount - 1));
		return;
	}

	if (g_alarm_field == kAlarmFieldHour) {
		alarm->hour = static_cast<uint8_t>(NextValue(alarm->hour, step, jump_to_tens, 0, 23));
	} else if (g_alarm_field == kAlarmFieldMinute) {
		alarm->minute = static_cast<uint8_t>(NextValue(alarm->minute, step, jump_to_tens, 0, 59));
	} else if (g_alarm_field == kAlarmFieldEnabled) {
		alarm->enabled ^= 1U;
	} else {
		alarm->days ^= static_cast<uint8_t>(1U << (g_alarm_field - kAlarmFieldFirstDay));
	}

	g_alarm_dirty = true;
}

void DrawAlarmEditor(bool hide)
{
	const Alarm *alarm = AlarmAt(g_alarm_index);
	char line1[16];
	char line2[8];

	snprintf(line1, sizeof(line1), "A%u %02u:%02u %s", g_alarm_index + 1U,
		 static_cast<unsigned int>(alarm->hour), static_cast<unsigned int>(alarm->minute),
		 alarm->enabled ? "P\305 " : "AV ");
	FormatDays(alarm->days, line2);

	if (hide) {
		if (g_alarm_field == 0) {
			BlankField(line1, 0);
		} else if (g_alarm_field == kAlarmFieldHour) {
			BlankField(line1, 3);
		} else if (g_alarm_field == kAlarmFieldMinute) {
			BlankField(line1, 6);
		} else if (g_alarm_field == kAlarmFieldEnabled) {
			memset(line1 + 9, ' ', 2);
		} else {
			line2[g_alarm_field - kAlarmFieldFirstDay] = ' ';
		}
	}

	DrawLines(line1, line2);
}

void DrawPairEditor(int64_t now, bool hide)
{
	char line1[16];
	char line2[16];

	if (g_pair_message != nullptr && now < g_pair_message_until) {
		DrawLines(g_pair_message, "");
		return;
	}

	FormatCode(g_pair_code, line1);
	if (g_pair_field == kPairFieldSlot) {
		snprintf(line2, sizeof(line2), "E%u %s", g_pair_slot + 1U,
			 DeviceCode(g_pair_slot)[0] != '\0' ? "SKRIV \326VER" : "LEDIG");
		if (hide) {
			line2[1] = ' ';
		}
	} else if (g_pair_field == kPairFieldSave) {
		snprintf(line2, sizeof(line2), "SPARA E%u: SW2", g_pair_slot + 1U);
		if (hide) {
			line2[0] = '\0';
		}
	} else {
		snprintf(line2, sizeof(line2), "SIFFRA %02u/%u",
			 static_cast<unsigned int>(g_pair_field + 1U), kPairingCodeDigits);
		if (hide) {
			const int digit = g_pair_field;

			line1[digit + (digit >= 4 ? 1 : 0) + (digit >= 7 ? 1 : 0)] = ' ';
		}
	}

	DrawLines(line1, line2);
}

void DrawDeviceList()
{
	char line1[16];
	char line2[16] = "-";
	const char *code = DeviceCode(g_list_index);
	const char *state = DeviceCommissioned(g_list_index) ? "ANSLUTEN"
			    : code[0] != '\0'                       ? "KOD SPARAD"
								    : "TOM";

	snprintf(line1, sizeof(line1), "E%u %s", g_list_index + 1U, state);
	if (code[0] != '\0') {
		snprintf(line2, sizeof(line2), "xxxx-xxx-%s", code + kPairingCodeDigits - 4);
	}
	DrawLines(line1, line2);
}

// Mode 4: byter var 2 s mellan tid, temperatur och luftfuktighet.
void DrawSensorCycle(uint32_t unix_time, int64_t now)
{
	const unsigned int view = static_cast<unsigned int>((now / kSensorViewMs) % 3);

	if (view == 0) {
		DrawBigClock(unix_time, false);
		return;
	}

	const SensorReading reading = SensorGet(0, unix_time);
	char value[16];

	if (view == 1) {
		if (!reading.temperature_valid) {
			DrawLines("TEMPERATUR", "VANTAR DATA");
			return;
		}
		const int t = reading.temperature_dc;
		const int abs_t = t < 0 ? -t : t;

		snprintf(value, sizeof(value), "%s%s%d.%dC",
			 reading.temperature_stale ? "GAMMAL " : "", t < 0 ? "-" : "",
			 abs_t / 10, abs_t % 10);
		DrawLines("TEMPERATUR", value);
	} else {
		if (!reading.humidity_valid) {
			snprintf(value, sizeof(value), "VANTAR DATA");
		} else {
			snprintf(value, sizeof(value), "%s%u %%",
				 reading.humidity_stale ? "GAMMAL " : "",
				 static_cast<unsigned int>(reading.humidity_pct));
		}
		DrawLines("LUFTFUKTIGHET", value);
	}
}

void PrintButtonLedState(uint8_t bitmask)
{
	printk("STATE sw1=%u led1=%u sw2=%u led2=%u sw3=%u led3=%u sw4=%u led4=%u mask=0x%02x\r\n",
	       (bitmask & 0x01U) ? 1U : 0U, (bitmask & 0x01U) ? 1U : 0U,
	       (bitmask & 0x02U) ? 1U : 0U, (bitmask & 0x02U) ? 1U : 0U,
	       (bitmask & 0x04U) ? 1U : 0U, (bitmask & 0x04U) ? 1U : 0U,
	       (bitmask & 0x08U) ? 1U : 0U, (bitmask & 0x08U) ? 1U : 0U,
	       static_cast<unsigned int>(bitmask));
}

void OnButtonStateChanged(uint8_t bitmask)
{
	static uint8_t previous;
	const uint8_t pressed = bitmask & static_cast<uint8_t>(~previous);

	previous = bitmask;
	PrintButtonLedState(bitmask);

	if (pressed) {
		atomic_or(&g_pressed, pressed);
		k_event_post(&g_events, kEventButton);
	}
}

void PrintMode()
{
	if (g_mode == kModeSetup) {
		printk("MODE mode=%u field=%s\r\n", static_cast<unsigned int>(kModeSetup),
		       FieldName(g_field));
	} else if (g_mode == kModeAlarm) {
		printk("MODE mode=%u alarm=%u field=%s\r\n", static_cast<unsigned int>(kModeAlarm),
		       g_alarm_index + 1U, AlarmFieldName(g_alarm_field));
	} else if (g_mode == kModePair) {
		printk("MODE mode=%u slot=%u field=%u\r\n", static_cast<unsigned int>(kModePair),
		       g_pair_slot + 1U, static_cast<unsigned int>(g_pair_field));
	} else {
		printk("MODE mode=%u\r\n", static_cast<unsigned int>(g_mode));
	}
}

void PairSave(int64_t now)
{
	const uint8_t saved_slot = g_pair_slot;
	const int err = DeviceCodeSave(g_pair_slot, g_pair_code);

	if (err) {
		g_pair_message = err == -EINVAL ? "UGILTIG KOD" : "SPARFEL";
		g_pair_field = 0;
	} else {
		g_pair_message = "KOD SPARAD";
		ResetPairEditor();
	}
	g_pair_message_until = now + kPairMessageMs;
	printk("PAIRCODE slot=%u saved=%u\r\n", saved_slot + 1U, err ? 0U : 1U);
}

// Returnerar true om en knapp hanterades.
bool HandleButtons(uint8_t pressed, int64_t now)
{
	if (!pressed) {
		return false;
	}

	if (pressed & kButtonMode) {
		// Mode 0 -> 1 -> 2 -> 3 -> 4 (sensorer) -> 50 (alarm) -> 60 (kod) -> 61 (lista) -> 99 (ställa) -> 0.
		if (g_mode == kModeAlarm) {
			SaveAlarmsIfDirty();
		}

		switch (g_mode) {
		case 0:
			g_mode = 1;
			break;
		case 1:
			g_mode = 2;
			break;
		case 2:
			g_mode = 3;
			break;
		case 3:
			g_mode = kModeSensors;
			break;
		case kModeSensors:
			g_mode = kModeAlarm;
			break;
		case kModeAlarm:
			g_mode = kModePair;
			break;
		case kModePair:
			g_mode = kModeList;
			break;
		case kModeList:
			g_mode = kModeSetup;
			break;
		default:
			g_mode = 0;
			break;
		}
		g_field = Field::Hours;
		g_alarm_field = 0;
		g_pair_message = nullptr;
		if (g_mode == kModePair) {
			ResetPairEditor();
		}
		PrintMode();
		return true;
	}

	if (!InEditMode()) {
		return true;
	}

	if (g_mode == kModeList) {
		if (pressed & (kButtonNext | kButtonUp)) {
			g_list_index = static_cast<uint8_t>((g_list_index + 1U) % kDeviceCount);
		}
		if (pressed & kButtonDown) {
			g_list_index = static_cast<uint8_t>((g_list_index + kDeviceCount - 1U) % kDeviceCount);
		}
		return true;
	}

	if (g_mode == kModePair) {
		if (g_pair_message != nullptr && now < g_pair_message_until) {
			return true;
		}
		g_pair_message = nullptr;

		if (pressed & kButtonNext) {
			if (g_pair_field == kPairFieldSave) {
				PairSave(now);
			} else {
				g_pair_field = static_cast<uint8_t>((g_pair_field + 1U) % kPairFieldCount);
			}
			PrintMode();
		}
		if (pressed & (kButtonUp | kButtonDown)) {
			const int step = (pressed & kButtonUp) ? 1 : -1;

			if (g_pair_field == kPairFieldSlot) {
				g_pair_slot = static_cast<uint8_t>(NextValue(g_pair_slot, step, false, 0,
									      kDeviceCount - 1));
			} else if (g_pair_field < kPairFieldSlot) {
				char *digit = &g_pair_code[g_pair_field];

				*digit = static_cast<char>('0' + NextValue(*digit - '0', step, false, 0, 9));
			}
		}
		return true;
	}

	if (pressed & kButtonNext) {
		if (g_mode == kModeSetup) {
			g_field = static_cast<Field>((static_cast<uint8_t>(g_field) + 1U) % kFieldCount);
		} else {
			g_alarm_field = static_cast<uint8_t>((g_alarm_field + 1U) % kAlarmFieldCount);
		}
		PrintMode();
	}
	if (pressed & (kButtonUp | kButtonDown)) {
		const int step = (pressed & kButtonUp) ? 1 : -1;

		if (g_mode == kModeSetup) {
			AdjustField(step);
		} else {
			AdjustAlarm(step);
		}
	}

	return true;
}

} // namespace

int UiTaskMain()
{
	int err;
#ifdef NRFNODE_MATTER_UI_THREAD
	err = ButtonHandlerAttach(OnButtonStateChanged);
#else
	err = ButtonHandlerInit(OnButtonStateChanged);
#endif
	if (err) {
		printk("ERROR init=button_led code=%d\r\n", err);
		return err;
	}

	err = RtcClockInit(0);
	if (err) {
		printk("ERROR init=rtc code=%d\r\n", err);
		return err;
	}

	printk("READY protocol=1 board=pan1780_evb baud=115200\r\n");
	printk("CLOCK restored=%u\r\n", RtcClockWasRestored() ? 1U : 0U);
	PrintButtonLedState(ButtonHandlerGetState());
	InitDisplay();

	err = AlarmsInit();
	if (err) {
		printk("ERROR init=alarm code=%d\r\n", err);
	}
	printk("ALARMS loaded=%u migrated=%u\r\n", AlarmsWereLoaded() ? 1U : 0U,
	       StorageMigrated() ? 1U : 0U);
	DevicesInit();
	PrintAllAlarms();

	const uint8_t saved_mode = DefaultModeLoad();

	if (saved_mode <= kModeSensors) {
		g_default_mode = saved_mode;
		g_mode = saved_mode;
	}
	printk("MODE default=%u\r\n", static_cast<unsigned int>(g_default_mode));
	PrintMode();

	int64_t last_activity = k_uptime_get();
	int64_t blink_epoch = last_activity;
	int64_t hold_next = 0;
	uint32_t last_printed = UINT32_MAX;
	uint32_t last_minute = RtcClockGet() / 60U;
	bool ringing = false;
	int64_t ring_start = 0;

	while (true) {
		k_event_wait(&g_events, kEventButton, true, K_MSEC(ringing ? 50 : 100));

		const int64_t now = k_uptime_get();
		uint8_t pressed = static_cast<uint8_t>(atomic_set(&g_pressed, 0));

		if (ringing && (pressed || now - ring_start >= kAlarmMaxMs)) {
			ringing = false;
			printk("ALARM_STOP reason=%s\r\n", pressed ? "button" : "timeout");
			dk_set_leds(ButtonHandlerGetState());
			// Knapptryckningen som stoppar alarmet ska inte också göra något annat.
			pressed = 0;
			last_activity = now;
		}

		if (HandleButtons(pressed, now)) {
			last_activity = now;
			blink_epoch = now;
			if (HoldRepeats() && (pressed & (kButtonUp | kButtonDown))) {
				hold_next = now + kHoldRepeatMs;
			}
		}

		// Hålls upp/ned inne hoppar värdet var sekund till nästa jämna tiotal.
		const uint8_t held = ButtonHandlerGetState();
		const uint8_t dir = held & (kButtonUp | kButtonDown);

		if (!ringing && HoldRepeats() && (dir == kButtonUp || dir == kButtonDown)) {
			last_activity = now;
			if (hold_next != 0 && now >= hold_next) {
				if (g_mode == kModeSetup) {
					AdjustField(dir == kButtonUp ? 1 : -1, true);
				} else {
					AdjustAlarm(dir == kButtonUp ? 1 : -1, true);
				}
				hold_next += kHoldRepeatMs;
			}
		} else {
			hold_next = 0;
		}

		if (InEditMode() && now - last_activity >= kSetupTimeoutMs) {
			SaveAlarmsIfDirty();
			g_mode = g_default_mode;
			printk("MODE mode=%u reason=timeout\r\n", static_cast<unsigned int>(g_mode));
		} else if (!InEditMode() && g_mode != g_default_mode &&
			   now - last_activity >= kSetupTimeoutMs) {
			// Ett visningsläge som lämnats orört i 10 s blir ny standard.
			g_default_mode = g_mode;
			printk("MODE default=%u\r\n", static_cast<unsigned int>(g_default_mode));
			// NVS hoppar över skrivning om värdet är oförändrat.
			if (DefaultModeSave(g_default_mode)) {
				printk("ERROR default_mode_save\r\n");
			}
		}

		const uint32_t unix_time = RtcClockGet();

		// Alarm kontrolleras när minuten byter, men inte medan man redigerar.
		if (unix_time / 60U != last_minute) {
			last_minute = unix_time / 60U;

			const int hit = ringing || InEditMode() ? -1 : AlarmsMatch(unix_time);

			if (hit >= 0) {
				ringing = true;
				ring_start = now;
				printk("ALARM_RING id=%d\r\n", hit + 1);
			}
		}

		const bool hide = InEditMode() && (((now - blink_epoch) / kBlinkHalfPeriodMs) % 2 == 1);
		char text[16];

		if (ringing) {
			dk_set_led(DK_LED1, ((now - ring_start) / kAlarmLedHalfPeriodMs) % 2 == 0);

			if (((now - ring_start) / kAlarmDisplayHalfPeriodMs) % 2 == 0) {
				DrawAlarmFrame();
			} else {
				DrawBigClock(unix_time, false);
			}
		} else if (g_mode == kModeAlarm) {
			DrawAlarmEditor(hide);
		} else if (g_mode == kModePair) {
			DrawPairEditor(now, hide);
		} else if (g_mode == kModeList) {
			DrawDeviceList();
		} else if (g_mode == kModeSensors) {
			DrawSensorCycle(unix_time, now);
		} else if (g_mode == kModeSetup) {
			char line2[16];

			snprintf(line2, sizeof(line2), "SET %s", FieldName(g_field));
			for (char *c = line2 + 4; *c; c++) {
				*c = static_cast<char>(*c - 'a' + 'A');
			}

			FormatTime(unix_time, hide, text, sizeof(text));
			DrawLines(text, line2);
		} else if (g_mode == 2) {
			DrawBigDate(unix_time);
		} else if (g_mode == 3) {
			DrawTimeDate(unix_time);
		} else {
			DrawBigClock(unix_time, g_mode == 1);
		}

		if (unix_time != last_printed) {
			last_printed = unix_time;
			RtcClockPersist();
			const uint32_t tod = unix_time % kSecondsPerDay;
			const Date date = DateFromDays(unix_time / kSecondsPerDay);

			printk("TIME unix=%u date=%04d-%02u-%02u hms=%02u:%02u:%02u wday=%s\r\n",
			       static_cast<unsigned int>(unix_time), date.year, date.month, date.day,
			       static_cast<unsigned int>(tod / 3600U),
			       static_cast<unsigned int>((tod / 60U) % 60U),
			       static_cast<unsigned int>(tod % 60U), kWeekdayProtocol[Weekday(unix_time)]);
		}
	}
}

#ifndef NRFNODE_MATTER_UI_THREAD
int main()
{
	return UiTaskMain();
}
#endif
