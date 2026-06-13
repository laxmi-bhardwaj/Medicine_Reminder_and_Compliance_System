// Medicine Reminder & Compliance System — ESP32

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <DHT.h>

// ─────────────────────────────────────────
//  OLED — Hardware SPI
// ─────────────────────────────────────────
#define OLED_MOSI   23
#define OLED_CLK    18
#define OLED_DC     16
#define OLED_CS      5
#define OLED_RESET  17

Adafruit_SSD1306 display(128, 64, &SPI, OLED_DC, OLED_RESET, OLED_CS);

// ─────────────────────────────────────────
//  PINS
// ─────────────────────────────────────────
#define BUZZER      25
#define BUTTON      26
#define IR_PIN      27
#define WATER_PIN   34
#define DHTPIN       4
#define DHTTYPE     DHT11

DHT dht(DHTPIN, DHTTYPE);

// ─────────────────────────────────────────
//  ENVIRONMENT THRESHOLDS
// ─────────────────────────────────────────
#define TEMP_MAX      35.0f
#define HUM_MAX       60.0f

// Water level threshold for confirming ingestion from a sensor drop.
// Picking up a glass lowers the reading, so a large drop is treated as a valid signal.
#define WATER_DROP_THRESH   20

// Hold the IR detection state briefly so a matching water-drop event can still confirm ingestion.
#define IR_LATCH_MS        2000UL

// ─────────────────────────────────────────
//  TIMING (ms)
// ─────────────────────────────────────────
#define BUTTON_DEBOUNCE_MS        300UL
#define DOUBLE_PRESS_WINDOW_MS   2000UL
#define SCHEDULE_VIEW_MS         5000UL
#define INGESTION_COOLDOWN_MS    5000UL
#define TAKEN_DISPLAY_MS         4000UL
#define DHT_READ_INTERVAL_MS     2000UL
#define DISPLAY_REFRESH_MS        200UL
#define BUZZER_BEEP_PERIOD_MS     600UL
#define EXPIRY_CHECK_INTERVAL_MS 10000UL

// Passive ingestion window used to match a dose to a nearby schedule slot.
#define PASSIVE_WINDOW_MIN        30

// ─────────────────────────────────────────
//  MEDICINE STRUCT
// ─────────────────────────────────────────
struct TimeOfDay {
  uint8_t hour;
  uint8_t minute;
};

struct Medicine {
  const char* name;
  TimeOfDay   schedule[4];
  uint8_t     scheduleCount;
  uint8_t     expiryDay;
  uint8_t     expiryMonth;
  uint16_t    expiryYear;
};

// ─────────────────────────────────────────
//  DEMO CLOCK
// ─────────────────────────────────────────
#define DEMO_START_HOUR    9
#define DEMO_START_MINUTE  57
#define DEMO_TIME_SCALE    10   // 1 real second = 10 simulated seconds

// ─────────────────────────────────────────
//  ★ USER CONFIGURATION ★
// ─────────────────────────────────────────
Medicine medicines[] = {
  {
    "Calcium Supplement",
    {{10, 40}},
    1,
    31, 12, 2025   // expired → triggers expiry alert immediately (demo)
  },
  {
    "Vitamin D",
    {{10, 2}},
    1,
    15, 6, 2026
  },
  {
    "XYZ med",
    {{10, 15}},
    1,
    25, 9, 2026
  }
};
const uint8_t MEDICINE_COUNT = sizeof(medicines) / sizeof(medicines[0]);

// ─────────────────────────────────────────
//  ALERT STATE MACHINE
// ─────────────────────────────────────────
enum AlertType {
  ALERT_NONE   = 0,
  ALERT_MED    = 1,    // lowest priority shown as number
  ALERT_ENV    = 2,
  ALERT_EXPIRY = 3     // highest priority
};

// Store a pending medication alert when a higher-priority alert is active.
static int8_t  pendingMedIndex = -1;   // -1 = none

struct AlertState {
  AlertType     type;
  uint8_t       medIndex;
  bool          buzzerActive;
  unsigned long startTime;
};

AlertState currentAlert = {ALERT_NONE, 0, false, 0};

// ─────────────────────────────────────────
//  PER-MEDICINE ACKNOWLEDGEMENT FLAGS
// ─────────────────────────────────────────
bool expiryAcknowledged[4] = {};

// Track whether the environment alert has been acknowledged until conditions recover.
bool envAcknowledged = false;

// ─────────────────────────────────────────
//  INGESTION STATE
// ─────────────────────────────────────────
struct IngestionState {
  bool          confirmed;
  uint8_t       medIndex;
  unsigned long confirmedAt;
};

IngestionState ingestion = {false, 0, 0};

// ─────────────────────────────────────────
//  SENSOR STATE
// ─────────────────────────────────────────
bool          prevIR         = HIGH;
int           prevWaterRaw   = -1;
bool          irLatched      = false;
unsigned long irLatchTime    = 0;

// ─────────────────────────────────────────
//  SCHEDULE TRACKING
// ─────────────────────────────────────────
bool scheduleFired[4][4]    = {};   // alarm has fired this day
bool scheduleLogged[4][4]   = {};   // ingestion was logged for this slot

// ─────────────────────────────────────────
//  DISPLAY STATE
// ─────────────────────────────────────────
unsigned long lastButtonPress      = 0xFFFFFFFF;
unsigned long prevButtonPress      = 0xFFFFFFFF;
unsigned long scheduleViewStart    = 0;
bool          showingScheduleView  = false;

// ─────────────────────────────────────────
//  TIMERS
// ─────────────────────────────────────────
unsigned long lastDHTRead        = 0;
unsigned long lastDisplayUpdate  = 0;
unsigned long lastBuzzerToggle   = 0;
unsigned long lastExpiryCheck    = 0;
bool          buzzerBeepState    = false;

// ─────────────────────────────────────────
//  ENV READINGS
// ─────────────────────────────────────────
float currentTemp = NAN;
float currentHum  = NAN;

// ─────────────────────────────────────────
//  SIMULATED CLOCK
// ─────────────────────────────────────────
uint8_t getCurrentHour() {
  unsigned long scaledSeconds = (millis() / 1000UL) * DEMO_TIME_SCALE;
  unsigned long totalMinutes  = DEMO_START_MINUTE + scaledSeconds / 60;
  unsigned long totalHours    = DEMO_START_HOUR   + totalMinutes  / 60;
  return (uint8_t)(totalHours % 24);
}

uint8_t getCurrentMinute() {
  unsigned long scaledSeconds = (millis() / 1000UL) * DEMO_TIME_SCALE;
  unsigned long totalMinutes  = DEMO_START_MINUTE + scaledSeconds / 60;
  return (uint8_t)(totalMinutes % 60);
}

uint8_t  getCurrentDay()   { return 1;    }
uint8_t  getCurrentMonth() { return 1;    }
uint16_t getCurrentYear()  { return 2026; }

// ─────────────────────────────────────────
//  HELPERS
// ─────────────────────────────────────────
bool isExpired(const Medicine& m) {
  uint16_t y  = getCurrentYear();
  uint8_t  mo = getCurrentMonth();
  uint8_t  d  = getCurrentDay();
  if (y  > m.expiryYear)  return true;
  if (y  < m.expiryYear)  return false;
  if (mo > m.expiryMonth) return true;
  if (mo < m.expiryMonth) return false;
  return (d > m.expiryDay);
}

// Raise an alert while respecting priority and parking lower-priority medication alerts.
void raiseAlert(AlertType type, uint8_t medIdx = 0) {
  if (type == ALERT_MED) {
    if (currentAlert.type > ALERT_MED) {
      // Higher-priority alert is active — park this med alert
      if (pendingMedIndex < 0) {
        pendingMedIndex = (int8_t)medIdx;
        Serial.printf("[PENDING] Med alert parked for %s\n", medicines[medIdx].name);
      }
      return;
    }
    if (currentAlert.type == ALERT_MED) {
      // Already showing a med alert; queue if different medicine
      if (pendingMedIndex < 0 && medIdx != currentAlert.medIndex) {
        pendingMedIndex = (int8_t)medIdx;
      }
      return;
    }
  }

  // Only raise if equal or higher priority than current
  if (type >= currentAlert.type) {
    currentAlert.type         = type;
    currentAlert.medIndex     = medIdx;
    currentAlert.buzzerActive = true;
    currentAlert.startTime    = millis();
    Serial.printf("[ALERT] Type=%d  Med=%s\n",
                  type,
                  (type == ALERT_MED || type == ALERT_EXPIRY)
                    ? medicines[medIdx].name : "ENV");
  }
}

// Clear the current alert and promote a pending medication alert if needed.
void clearAlert() {
  AlertType cleared = currentAlert.type;
  currentAlert.type         = ALERT_NONE;
  currentAlert.buzzerActive = false;
  digitalWrite(BUZZER, LOW);
  buzzerBeepState = false;
  Serial.printf("[CLEAR] Alert type=%d cleared\n", cleared);

  // After a higher-priority alert clears, promote a pending med alert
  if (pendingMedIndex >= 0) {
    uint8_t idx = (uint8_t)pendingMedIndex;
    pendingMedIndex = -1;
    // Only promote if this med's slot was fired but not yet logged
    currentAlert.type         = ALERT_MED;
    currentAlert.medIndex     = idx;
    currentAlert.buzzerActive = true;
    currentAlert.startTime    = millis();
    Serial.printf("[PROMOTE] Med alert promoted for %s\n", medicines[idx].name);
  }
}

void logIngestion(uint8_t medIdx, uint8_t slotIdx) {
  scheduleLogged[medIdx][slotIdx] = true;
  Serial.printf("[CALENDAR] TAKEN: %s at %02d:%02d on %02d/%02d/%04d\n",
                medicines[medIdx].name,
                getCurrentHour(), getCurrentMinute(),
                getCurrentDay(), getCurrentMonth(), getCurrentYear());
}

// ─────────────────────────────────────────
//  Find the closest schedule slot within the passive confirmation window.
// ─────────────────────────────────────────
int8_t findNearbySlot(uint8_t medIdx, uint8_t curH, uint8_t curM) {
  int16_t curTotal = (int16_t)curH * 60 + curM;
  for (uint8_t s = 0; s < medicines[medIdx].scheduleCount; s++) {
    if (scheduleLogged[medIdx][s]) continue;   // already logged
    int16_t slotTotal = (int16_t)medicines[medIdx].schedule[s].hour * 60
                      + medicines[medIdx].schedule[s].minute;
    int16_t diff = curTotal - slotTotal;
    if (diff < -PASSIVE_WINDOW_MIN || diff > PASSIVE_WINDOW_MIN) continue;
    return (int8_t)s;
  }
  return -1;
}

// ─────────────────────────────────────────
//  FORWARD DECLARATIONS
// ─────────────────────────────────────────
void updateDisplay(uint8_t h, uint8_t m, int waterRaw);

// ─────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== MedGuard Boot ===");

  SPI.begin(OLED_CLK, -1, OLED_MOSI, OLED_CS);
  if (!display.begin(SSD1306_SWITCHCAPVCC)) {
    Serial.println("OLED init failed — continuing without display");
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("MedGuard v2.0");
  display.println("Initializing...");
  display.display();

  pinMode(BUZZER, OUTPUT);
  pinMode(BUTTON, INPUT_PULLUP);
  pinMode(IR_PIN, INPUT);
  digitalWrite(BUZZER, LOW);

  dht.begin();
  delay(1500);
  currentTemp = dht.readTemperature();
  currentHum  = dht.readHumidity();

  memset(scheduleFired,    0, sizeof(scheduleFired));
  memset(scheduleLogged,   0, sizeof(scheduleLogged));
  memset(expiryAcknowledged, 0, sizeof(expiryAcknowledged));
  envAcknowledged = false;

  Serial.println("System ready.");
}

// ─────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────
void loop() {
  unsigned long now  = millis();
  uint8_t curH = getCurrentHour();
  uint8_t curM = getCurrentMinute();

  // ── 1. BUTTON ──────────────────────────────────────────────
  if (digitalRead(BUTTON) == LOW && (now - lastButtonPress > BUTTON_DEBOUNCE_MS)) {

    bool isDoublePress = (prevButtonPress != 0xFFFFFFFF) &&
                         (now - prevButtonPress < DOUBLE_PRESS_WINDOW_MS);
    prevButtonPress = lastButtonPress;
    lastButtonPress = now;

    if (currentAlert.buzzerActive) {
      // ── Active alarm: button = dismiss (no log)
      AlertType t = currentAlert.type;

      if (t == ALERT_EXPIRY) {
        expiryAcknowledged[currentAlert.medIndex] = true;
        Serial.printf("[ACK] Expiry ack for %s\n",
                      medicines[currentAlert.medIndex].name);
      } else if (t == ALERT_ENV) {
        envAcknowledged = true;
        Serial.println("[ACK] Env alert acknowledged");
      } else if (t == ALERT_MED) {
        Serial.println("[BUTTON] Med alarm dismissed — no log");
      }

      clearAlert();

    } else {
      // ── No active alarm ──────────────────────────────────
      if (isDoublePress) {
        showingScheduleView = true;
        scheduleViewStart   = now;
        Serial.println("[UI] Schedule view triggered");
      } else {
        // Single press with no alarm → manual reset (keep for dev use)
        // In production you may remove this branch or repurpose it
        Serial.println("[BUTTON] Single press — no active alarm");
      }
    }
  }

  if (showingScheduleView && (now - scheduleViewStart >= SCHEDULE_VIEW_MS)) {
    showingScheduleView = false;
  }

  // ── 2. DHT READ ────────────────────────────────────────────
  if (now - lastDHTRead >= DHT_READ_INTERVAL_MS) {
    lastDHTRead = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) currentTemp = t;
    if (!isnan(h)) currentHum  = h;

    if (!isnan(currentTemp) && !isnan(currentHum)) {
      if (currentTemp <= TEMP_MAX && currentHum <= HUM_MAX) {
        if (envAcknowledged) {
          envAcknowledged = false;
          Serial.println("[ENV] Conditions normalised — ack reset");
        }
      }
    }
  }

  // ── 3. EXPIRY CHECK ────────────────────────────────────────
  if (now - lastExpiryCheck >= EXPIRY_CHECK_INTERVAL_MS) {
    lastExpiryCheck = now;
    for (uint8_t i = 0; i < MEDICINE_COUNT; i++) {
      if (expiryAcknowledged[i]) continue;
      if (isExpired(medicines[i])) {
        raiseAlert(ALERT_EXPIRY, i);
      }
    }
  }

  // ── 4. ENVIRONMENT CHECK ───────────────────────────────────
  if (!isnan(currentTemp) && !isnan(currentHum)) {
    bool envBad = (currentTemp > TEMP_MAX || currentHum > HUM_MAX);

    if (envBad && !envAcknowledged && currentAlert.type < ALERT_ENV) {
      raiseAlert(ALERT_ENV);
    }

    // Auto-clear env alert if conditions normalise (no button needed)
    if (currentAlert.type == ALERT_ENV && !envBad) {
      Serial.println("[ENV] Auto-clearing: conditions OK");
      clearAlert();
    }
  }

  // ── 5. SCHEDULE CHECK ──────────────────────────────────────
  for (uint8_t i = 0; i < MEDICINE_COUNT; i++) {
    // Skip expired medicines (user has been warned)
    // if (expiryAcknowledged[i] || isExpired(medicines[i])) continue;

    for (uint8_t s = 0; s < medicines[i].scheduleCount; s++) {
      if (!scheduleFired[i][s] &&
          curH == medicines[i].schedule[s].hour &&
          curM == medicines[i].schedule[s].minute) {
        scheduleFired[i][s] = true;
        if (scheduleLogged[i][s]) {
            // already taken passively — silent, no alarm
        } else {
            raiseAlert(ALERT_MED, i);  // only rings if NOT already logged
        }
        Serial.printf("[SCHEDULE] %s slot %d fired\n", medicines[i].name, s);
      }
    }
  }

  // ── 6. SENSOR READ ─────────────────────────────────────────
  bool currentIR = digitalRead(IR_PIN);
  int  waterRaw  = analogRead(WATER_PIN);

  // IR edge: HIGH→LOW = object/hand detected
  if (prevIR == HIGH && currentIR == LOW) {
    irLatched   = true;
    irLatchTime = now;
    Serial.println("[IR] Edge detected — latch started");
  }
  // Expire the latch after IR_LATCH_MS
  if (irLatched && (now - irLatchTime > IR_LATCH_MS)) {
    irLatched = false;
    Serial.println("[IR] Latch expired");
  }

  // Water drop: reading decreased by threshold vs previous sample
  bool waterDrop = false;
  if (prevWaterRaw >= 0) {
    waterDrop = ((prevWaterRaw - waterRaw) >= WATER_DROP_THRESH);
    if (waterDrop) Serial.printf("[WATER] Drop detected: %d -> %d\n", prevWaterRaw, waterRaw);
  }

  prevIR       = currentIR;
  prevWaterRaw = waterRaw;

  // Ingestion = IR latch active AND water drop in this tick
  bool ingestionDetected = irLatched && waterDrop &&
                           (now - ingestion.confirmedAt > INGESTION_COOLDOWN_MS);
  if (ingestionDetected) irLatched = false;  // consume the latch

  // ── 7. INGESTION DETECTION (alarm active) ─────────────────
  if (currentAlert.type == ALERT_MED && !ingestion.confirmed && ingestionDetected) {

    uint8_t medIdx = currentAlert.medIndex;
    int8_t slotIdx = -1;
    for (uint8_t s = 0; s < medicines[medIdx].scheduleCount; s++) {
      if (scheduleFired[medIdx][s] && !scheduleLogged[medIdx][s]) {
        slotIdx = (int8_t)s;
        break;
      }
    }

    ingestion.confirmed   = true;
    ingestion.medIndex    = medIdx;
    ingestion.confirmedAt = now;

    if (slotIdx >= 0) logIngestion(medIdx, (uint8_t)slotIdx);
    clearAlert();
    Serial.println("[INGESTION] Confirmed via alarm — logged.");
  }

  // ── 8. PASSIVE INGESTION (±30 min window, no active alarm) ─
  if (currentAlert.type != ALERT_MED && ingestionDetected) {
    for (uint8_t i = 0; i < MEDICINE_COUNT; i++) {
      // if (expiryAcknowledged[i] || isExpired(medicines[i])) continue;
      int8_t slotIdx = findNearbySlot(i, curH, curM);
      if (slotIdx >= 0) {
        ingestion.confirmed   = true;
        ingestion.medIndex    = i;
        ingestion.confirmedAt = now;
        logIngestion(i, (uint8_t)slotIdx);
        Serial.printf("[INGESTION] Passive window — %s slot %d logged.\n",
                      medicines[i].name, slotIdx);
        break;
      }
    }
  }

  // Auto-clear "TAKEN" display
  if (ingestion.confirmed && (now - ingestion.confirmedAt > TAKEN_DISPLAY_MS)) {
    ingestion.confirmed = false;
  }

  // ── 9. NON-BLOCKING BUZZER ────────────────────────────────
  if (currentAlert.buzzerActive) {
    if (now - lastBuzzerToggle >= BUZZER_BEEP_PERIOD_MS) {
      lastBuzzerToggle = now;
      buzzerBeepState  = !buzzerBeepState;
      digitalWrite(BUZZER, buzzerBeepState ? HIGH : LOW);
    }
  } else {
    digitalWrite(BUZZER, LOW);
    buzzerBeepState = false;
  }

  // ── 10. DISPLAY ───────────────────────────────────────────
  if (now - lastDisplayUpdate >= DISPLAY_REFRESH_MS) {
    lastDisplayUpdate = now;
    updateDisplay(curH, curM, waterRaw);
  }
}

// ─────────────────────────────────────────
//  HELPERS: NEXT ALARM STRING
// ─────────────────────────────────────────
// Returns the medicine index and slot of the next pending alarm.
// Returns false if none found.
bool getNextAlarm(uint8_t curH, uint8_t curM,
                  uint8_t& outMed, uint8_t& outSlot) {
  int16_t curTotal  = (int16_t)curH * 60 + curM;
  int16_t bestDiff  = 32767;
  bool    found     = false;

  for (uint8_t i = 0; i < MEDICINE_COUNT; i++) {
    // if (expiryAcknowledged[i] || isExpired(medicines[i])) continue;
    for (uint8_t s = 0; s < medicines[i].scheduleCount; s++) {
      if (scheduleFired[i][s] || scheduleLogged[i][s]) continue;
      int16_t slotTotal = (int16_t)medicines[i].schedule[s].hour * 60
                        + medicines[i].schedule[s].minute;
      int16_t diff = slotTotal - curTotal;
      if (diff < 0) diff += 1440;   // wrap to next day
      if (diff < bestDiff) {
        bestDiff = diff;
        outMed   = i;
        outSlot  = s;
        found    = true;
      }
    }
  }
  return found;
}

// ─────────────────────────────────────────
//  DISPLAY RENDERER
// ─────────────────────────────────────────
void updateDisplay(uint8_t h, uint8_t m, int waterRaw) {
  display.clearDisplay();

  // ── Active Alert ──────────────────────────────────────────
  if (currentAlert.type != ALERT_NONE) {
    const Medicine& med = medicines[currentAlert.medIndex];
    
    static bool blink = false;
    blink = !blink;
    if (blink) display.drawRect(0, 0, 128, 64, SSD1306_WHITE);

    switch (currentAlert.type) {
      
      case ALERT_MED:
        display.setTextSize(2);
        display.setCursor(8, 2);
        display.println("Med Time!");
        display.setTextSize(1);
        display.setCursor(4, 28);
        display.print("Take: ");
        display.println(med.name);
        display.setCursor(4, 40);
        display.println("IR+Water=confirm");
        display.setCursor(4, 52);
        display.println("Btn=skip (no log)");
        break;

      case ALERT_EXPIRY:
        display.setTextSize(1);
        display.setCursor(4, 2);
        display.println("!! EXPIRY ALERT !!");
        display.drawFastHLine(0, 12, 128, SSD1306_WHITE);
        display.setCursor(4, 16);
        display.println(med.name);
        display.setCursor(4, 28);
        display.printf("Exp: %02d/%02d/%04d",
                        med.expiryDay, med.expiryMonth, med.expiryYear);
        display.setCursor(4, 40);
        display.println("Replace medicine!");
        display.setCursor(4, 52);
        display.println("Btn to acknowledge");
        break;

      case ALERT_ENV:
        display.setTextSize(1);
        display.setCursor(4, 2);
        display.println("!! ENV WARNING !!");
        display.drawFastHLine(0, 12, 128, SSD1306_WHITE);
        display.setCursor(4, 16);
        display.println("Unsafe storage env");
        if (!isnan(currentTemp)) {
          display.setCursor(4, 28);
          display.printf("T:%.1fC lim:%dC", currentTemp, (int)TEMP_MAX);
        }
        if (!isnan(currentHum)) {
          display.setCursor(4, 40);
          display.printf("H:%.1f%% lim:%d%%", currentHum, (int)HUM_MAX);
        }
        display.setCursor(4, 52);
        display.println("Btn to acknowledge");
        break;

      default: break;
    }
    display.display();
    return;
  }

  // ── Ingestion Confirmed ───────────────────────────────────
  if (ingestion.confirmed) {
    display.setTextSize(2);
    display.setCursor(10, 6);
    display.println("Taken!");
    display.setTextSize(1);
    display.setCursor(4, 34);
    display.print(medicines[ingestion.medIndex].name);
    display.setCursor(4, 46);
    display.println("Logged to calendar");
    display.display();
    return;
  }

  // ── Full schedule view (double-press) ────────────
  if (showingScheduleView) {
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("-- Full Schedule --");
    display.drawFastHLine(0, 10, 128, SSD1306_WHITE);
    uint8_t yPos = 13;
    for (uint8_t i = 0; i < MEDICINE_COUNT && yPos < 55; i++) {
      for (uint8_t s = 0; s < medicines[i].scheduleCount && yPos < 55; s++) {
        display.setCursor(0, yPos);
        const char* status = scheduleLogged[i][s] ? "[ok]"
                           : scheduleFired[i][s]  ? "[skip]"
                                                   : "[pend]";
        display.printf("%s %02d:%02d %s",
                        medicines[i].name,
                        medicines[i].schedule[s].hour,
                        medicines[i].schedule[s].minute,
                        status);
        yPos += 10;
      }
    }
    // Countdown bar
    unsigned long elapsed = millis() - scheduleViewStart;
    uint8_t barW = (uint8_t)((SCHEDULE_VIEW_MS - elapsed) * 128 / SCHEDULE_VIEW_MS);
    display.drawFastHLine(0, 63, barW, SSD1306_WHITE);
    display.display();
    return;
  }

  // ── Idle view — show the next upcoming alarm ─────────
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.printf("MedGuard  %02d:%02d", h, m);
  display.drawFastHLine(0, 10, 128, SSD1306_WHITE);

  // Temp/Humidity
  display.setCursor(0, 13);
  if (!isnan(currentTemp) && !isnan(currentHum)) {
    display.printf("T:%.1fC  H:%.1f%%", currentTemp, currentHum);
  } else {
    display.println("Sensor warming up");
  }
  display.drawFastHLine(0, 23, 128, SSD1306_WHITE);

  // Next alarm
  uint8_t nextMed = 0, nextSlot = 0;
  display.setCursor(0, 26);
  if (getNextAlarm(h, m, nextMed, nextSlot)) {
    display.println("Next alarm:");
    display.setCursor(0, 36);
    display.printf(" %s", medicines[nextMed].name);
    display.setCursor(0, 46);
    display.printf(" @ %02d:%02d",
                    medicines[nextMed].schedule[nextSlot].hour,
                    medicines[nextMed].schedule[nextSlot].minute);
  } else {
    display.println("All doses done!");
    display.setCursor(0, 36);
    display.println("Dbl-press for sched");
  }

  display.setCursor(0, 56);
  display.printf("W:%d IR:%s", waterRaw, digitalRead(IR_PIN) == LOW ? "ON" : "--");

  display.display();
}