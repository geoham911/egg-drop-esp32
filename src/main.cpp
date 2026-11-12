#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_SH110X.h>
#include <ESP32Servo.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>
#include <stdio.h>
#include "wifi_credentials.h"

/**
 * Egg Drop controller for an ESP32-C3 DevKitC-02.
 *
 * Configure the Wi-Fi credentials and verify the pin assignments below against
 * the wiring before uploading. The HTTP controls are intended for a trusted
 * local network; do not expose this unauthenticated server to the internet.
 */

namespace {

// Default ESP32-C3 DevKitC-02 pin choices; change these to match your wiring.
constexpr uint8_t I2C_SDA_PIN = 8;
constexpr uint8_t I2C_SCL_PIN = 9;
constexpr uint8_t SERVO_PIN = 4;
constexpr uint8_t AUDIO_PIN = 5;
constexpr uint8_t RGB_RING_DATA_PIN = 6;
constexpr uint16_t RGB_RING_LED_COUNT = 24;
constexpr uint8_t RGB_RING_BRIGHTNESS = 48;

// Common SH1106 I2C addresses. Startup probes these and uses the one detected.
const uint8_t DISPLAY_I2C_ADDRESSES[] = {0x3C, 0x3D};
constexpr size_t DISPLAY_I2C_ADDRESS_COUNT =
    sizeof(DISPLAY_I2C_ADDRESSES) / sizeof(DISPLAY_I2C_ADDRESSES[0]);

// Change this to true if the linkage moves in the opposite direction.
constexpr bool REVERSE_SERVO_DIRECTION = false;
constexpr int SERVO_CLOSED_ANGLE = 90;
constexpr int SERVO_OPEN_ANGLE = 150;
constexpr int SERVO_MIN_PULSE_US = 500;
constexpr int SERVO_MAX_PULSE_US = 2400;

constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 12000;
constexpr unsigned long WIFI_RETRY_INTERVAL_MS = 15000;
constexpr unsigned long DISPLAY_REFRESH_INTERVAL_MS = 1000;
constexpr unsigned long TRIGGER_COUNTDOWN_MS = 5000;
constexpr unsigned long SERVO_ACTIVATION_MS = 2000;
constexpr unsigned long COUNTDOWN_BEEP_INTERVAL_MS = 1000;
constexpr unsigned long COUNTDOWN_BEEP_DURATION_MS = 120;
constexpr uint8_t COUNTDOWN_BEEP_COUNT = 5;
constexpr unsigned long RGB_RING_ROTATION_MS = 1000;
constexpr unsigned long GREEN_FLASH_INTERVAL_MS = 250;

enum class TriggerState {
  Idle,
  Countdown,
  ServoActive,
};

Adafruit_SH1106G display(128, 64, &Wire, -1);
Adafruit_NeoPixel rgbRing(RGB_RING_LED_COUNT, RGB_RING_DATA_PIN,
                          NEO_GRB + NEO_KHZ800);
WebServer webServer(80);
Servo servo;

bool displayReady = false;
bool servoReady = false;
bool servoIsOpen = false;
TriggerState triggerState = TriggerState::Idle;
unsigned long triggerStateStartedAt = 0;
uint8_t countdownBeepsPlayed = 0;
uint16_t lastCountdownPixel = RGB_RING_LED_COUNT;
bool greenRingIsOn = false;
unsigned long nextWifiAttemptAt = 0;
unsigned long lastDisplayRefreshAt = 0;

/**
 * Convert a logical open/closed position to the physical servo angle.
 * The reversal option changes direction without changing button behavior.
 */
int servoAngleForPosition(int logicalAngle) {
  if (!REVERSE_SERVO_DIRECTION) {
    return logicalAngle;
  }

  return logicalAngle == SERVO_CLOSED_ANGLE ? SERVO_OPEN_ANGLE
                                            : SERVO_CLOSED_ANGLE;
}

/** Move to the closed/default position and record the current state. */
void closeServo() {
  if (!servoReady) {
    return;
  }

  servo.write(servoAngleForPosition(SERVO_CLOSED_ANGLE));
  servoIsOpen = false;
  Serial.println("Servo set to closed/default position.");
}

/** Move to the fully open position and record the current state. */
void openServo() {
  if (!servoReady) {
    return;
  }

  servo.write(servoAngleForPosition(SERVO_OPEN_ANGLE));
  servoIsOpen = true;
  Serial.println("Servo set to open position.");
}

/** Rotate one red pixel around the ring once per second during countdown. */
void updateCountdownRing(unsigned long elapsed) {
  const uint16_t pixel =
      (elapsed % RGB_RING_ROTATION_MS) * RGB_RING_LED_COUNT /
      RGB_RING_ROTATION_MS;
  if (pixel == lastCountdownPixel) {
    return;
  }

  rgbRing.clear();
  rgbRing.setPixelColor(pixel, rgbRing.Color(255, 0, 0));
  rgbRing.show();
  lastCountdownPixel = pixel;
}

/** Set every ring pixel to green or turn the ring off. */
void setRingGreen(bool on) {
  const uint32_t color = on ? rgbRing.Color(0, 255, 0) : 0;
  for (uint16_t pixel = 0; pixel < RGB_RING_LED_COUNT; ++pixel) {
    rgbRing.setPixelColor(pixel, color);
  }
  rgbRing.show();
  greenRingIsOn = on;
}

/** Turn off every pixel when a trigger cycle ends or is cancelled. */
void clearRgbRing() {
  rgbRing.clear();
  rgbRing.show();
  greenRingIsOn = false;
  lastCountdownPixel = RGB_RING_LED_COUNT;
}

/** Advance the countdown and servo activation without blocking the web server. */
void updateTriggerSequence() {
  const unsigned long elapsed = millis() - triggerStateStartedAt;

  if (triggerState == TriggerState::Countdown) {
    updateCountdownRing(elapsed);

    if (countdownBeepsPlayed < COUNTDOWN_BEEP_COUNT &&
        elapsed >= countdownBeepsPlayed * COUNTDOWN_BEEP_INTERVAL_MS) {
      const unsigned int frequency =
          countdownBeepsPlayed == COUNTDOWN_BEEP_COUNT - 1 ? 1200 : 800;
      tone(AUDIO_PIN, frequency, COUNTDOWN_BEEP_DURATION_MS);
      ++countdownBeepsPlayed;
    }

    if (elapsed >= TRIGGER_COUNTDOWN_MS) {
      openServo();
      triggerState = TriggerState::ServoActive;
      triggerStateStartedAt = millis();
      setRingGreen(true);
      Serial.println("Countdown complete; servo activated.");
    }
  } else if (triggerState == TriggerState::ServoActive) {
    if (elapsed >= SERVO_ACTIVATION_MS) {
      closeServo();
      clearRgbRing();
      triggerState = TriggerState::Idle;
      triggerStateStartedAt = 0;
      Serial.println("Servo activation complete; sequence reset.");
    } else {
      const bool shouldBeOn =
          (elapsed / GREEN_FLASH_INTERVAL_MS) % 2 == 0;
      if (shouldBeOn != greenRingIsOn) {
        setRingGreen(shouldBeOn);
      }
    }
  }
}

/** Return the remaining countdown in whole seconds, rounded up. */
unsigned long countdownSecondsRemaining() {
  const unsigned long elapsed = millis() - triggerStateStartedAt;
  if (elapsed >= TRIGGER_COUNTDOWN_MS) {
    return 0;
  }

  return (TRIGGER_COUNTDOWN_MS - elapsed + 999) / 1000;
}

/** Render the current connection status and local IP address on the OLED. */
void updateDisplay() {
  if (!displayReady) {
    return;
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.println("ESP32 Egg Drop");
  display.println();

  if (WiFi.status() == WL_CONNECTED) {
    display.println("Wi-Fi connected");
    display.print("IP: ");
    display.println(WiFi.localIP());
  } else {
    display.println("Wi-Fi disconnected");
    display.println("Retrying...");
  }

  display.println();
  if (triggerState == TriggerState::Countdown) {
    display.print("Trigger in: ");
    display.print(countdownSecondsRemaining());
    display.println(" sec");
  } else if (servoIsOpen) {
    display.println("Servo: OPEN");
  } else {
    display.println("Servo: CLOSED");
  }
  display.display();
}

/**
 * Try each configured network in order. Return after the first connection;
 * otherwise leave Wi-Fi disconnected for the periodic retry in loop().
 */
bool connectToKnownNetworks() {
  WiFi.mode(WIFI_STA);

  for (size_t i = 0; i < WIFI_NETWORK_COUNT; ++i) {
    const WifiCredential& network = WIFI_NETWORKS[i];
    if (network.ssid[0] == '\0') {
      continue;
    }

    Serial.print("Connecting to Wi-Fi network: ");
    Serial.println(network.ssid);
    WiFi.disconnect(false);
    WiFi.begin(network.ssid, network.password);

    const unsigned long startedAt = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - startedAt < WIFI_CONNECT_TIMEOUT_MS) {
      delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("Connected. IP address: ");
      Serial.println(WiFi.localIP());
      return true;
    }

    Serial.println("Connection timed out; trying the next network.");
  }

  Serial.println("No configured Wi-Fi network is available.");
  WiFi.disconnect(false);
  return false;
}

/** Serve the small browser UI used to trigger and reset the servo. */
void handleRoot() {
  static const char PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 Egg Drop</title>
  <style>
    body { font: 1.1rem system-ui, sans-serif; max-width: 32rem; margin: 3rem auto; padding: 0 1rem; text-align: center; }
    button { border: 0; border-radius: .5rem; color: white; cursor: pointer; font-size: 1.1rem; margin: .5rem; padding: 1rem 2rem; }
    #trigger { background: #b42318; } #reset { background: #344054; }
    #status { min-height: 1.5em; }
  </style>
</head>
<body>
  <h1>ESP32 Egg Drop</h1>
  <p>Servo control</p>
  <button id="trigger" type="button" onclick="sendCommand('/trigger')">Trigger</button>
  <button id="reset" type="button" onclick="sendCommand('/reset')">Reset</button>
  <p id="status" role="status" aria-live="polite"></p>
  <script>
    const triggerButton = document.getElementById('trigger');
    const status = document.getElementById('status');

    async function refreshStatus() {
      try {
        const response = await fetch('/status');
        if (!response.ok) return;
        const state = await response.json();
        if (state.state === 'countdown') {
          status.textContent = `Opening in ${state.secondsRemaining} second(s).`;
          triggerButton.disabled = true;
        } else if (state.state === 'servo_active') {
          status.textContent = 'Servo activated.';
          triggerButton.disabled = true;
        } else {
          status.textContent = 'Ready.';
          triggerButton.disabled = false;
        }
      } catch (_) {
        status.textContent = 'Could not read controller status.';
      }
    }

    async function sendCommand(path) {
      status.textContent = 'Sending command...';
      try {
        const response = await fetch(path, { method: 'POST' });
        const message = await response.text();
        if (!response.ok) status.textContent = message;
        await refreshStatus();
      } catch (_) {
        status.textContent = 'Could not reach the ESP32.';
      }
    }

    setInterval(refreshStatus, 250);
    refreshStatus();
  </script>
</body>
</html>
)HTML";

  webServer.send_P(200, "text/html; charset=utf-8", PAGE);
}

/** Start a five-second countdown for a new trigger cycle. */
void handleTrigger() {
  if (!servoReady) {
    webServer.send(503, "text/plain", "Servo is not available.");
    return;
  }

  if (triggerState != TriggerState::Idle) {
    webServer.send(409, "text/plain", "A trigger cycle is already in progress.");
    return;
  }

  triggerState = TriggerState::Countdown;
  triggerStateStartedAt = millis();
  countdownBeepsPlayed = 0;
  Serial.println("Trigger received; five-second countdown started.");
  webServer.send(202, "text/plain", "Five-second countdown started.");
}

/** Report the current trigger cycle state to the browser UI. */
void handleStatus() {
  char response[64];

  if (triggerState == TriggerState::Countdown) {
    snprintf(response, sizeof(response),
             "{\"state\":\"countdown\",\"secondsRemaining\":%lu}",
             countdownSecondsRemaining());
  } else if (triggerState == TriggerState::ServoActive) {
    snprintf(response, sizeof(response), "{\"state\":\"servo_active\"}");
  } else {
    snprintf(response, sizeof(response), "{\"state\":\"idle\"}");
  }

  webServer.send(200, "application/json", response);
}

/** Cancel any pending cycle and return the servo to its default state. */
void handleReset() {
  if (!servoReady) {
    webServer.send(503, "text/plain", "Servo is not available.");
    return;
  }

  closeServo();
  noTone(AUDIO_PIN);
  clearRgbRing();
  triggerState = TriggerState::Idle;
  triggerStateStartedAt = 0;
  countdownBeepsPlayed = 0;
  webServer.send(200, "text/plain", "Sequence cancelled; servo reset.");
}

/** Initialize the addressable RGB ring with a conservative brightness. */
void initializeRgbRing() {
  rgbRing.begin();
  rgbRing.setBrightness(RGB_RING_BRIGHTNESS);
  clearRgbRing();
}

/** Initialize the SH1106 display and show connection progress. */
void initializeDisplay() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);

  uint8_t detectedAddress = 0;
  for (size_t i = 0; i < DISPLAY_I2C_ADDRESS_COUNT; ++i) {
    const uint8_t address = DISPLAY_I2C_ADDRESSES[i];
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      detectedAddress = address;
      break;
    }
  }

  if (detectedAddress == 0) {
    Serial.println("No I2C device found at 0x3C or 0x3D.");
    Serial.println("Check OLED power, GND, SDA/SCL wiring and GPIO settings.");
    return;
  }

  Serial.print("I2C display detected at address 0x");
  Serial.println(detectedAddress, HEX);
  displayReady = display.begin(detectedAddress, true);

  if (!displayReady) {
    Serial.println("SH1106 display setup failed after I2C detection.");
    Serial.println("Check display type, resolution and library compatibility.");
    return;
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.println("Starting ESP32...");
  display.display();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println("Starting ESP32 Egg Drop controller.");

  initializeRgbRing();
  initializeDisplay();

  // Set a standard 50 Hz servo PWM period before attaching the SG90.
  servo.setPeriodHertz(50);
  servoReady =
      servo.attach(SERVO_PIN, SERVO_MIN_PULSE_US, SERVO_MAX_PULSE_US) >= 0;
  if (servoReady) {
    closeServo();
  } else {
    Serial.println("Servo initialization failed.");
  }

  if (!connectToKnownNetworks()) {
    nextWifiAttemptAt = millis() + WIFI_RETRY_INTERVAL_MS;
  }

  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/trigger", HTTP_POST, handleTrigger);
  webServer.on("/status", HTTP_GET, handleStatus);
  webServer.on("/reset", HTTP_POST, handleReset);
  webServer.onNotFound([]() {
    webServer.send(404, "text/plain", "Not found.");
  });
  webServer.begin();
  Serial.println("HTTP server started on port 80.");

  updateDisplay();
}

void loop() {
  webServer.handleClient();
  updateTriggerSequence();

  // Retry both configured networks after a disconnect or failed startup.
  // Avoid blocking Wi-Fi reconnect attempts during the timed servo sequence.
  if (triggerState == TriggerState::Idle &&
      WiFi.status() != WL_CONNECTED && millis() >= nextWifiAttemptAt) {
    connectToKnownNetworks();
    nextWifiAttemptAt = millis() + WIFI_RETRY_INTERVAL_MS;
  }

  // Refresh the OLED periodically so Wi-Fi and servo status stay current.
  if (millis() - lastDisplayRefreshAt >= DISPLAY_REFRESH_INTERVAL_MS) {
    lastDisplayRefreshAt = millis();
    updateDisplay();
  }
}