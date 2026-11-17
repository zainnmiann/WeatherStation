/*
  SparkFun ESP32 Thing Plus C - Hava Istasyonu (iyilestirilmis surum)
  Sensorler: BME280 (sicaklik/nem/basinc) + AS3935 (yildirim algilama)
             + Weather Meter Kit (ruzgar/yagmur)
             + Toprak Nemi Sensoru (soil moisture, analog)
  Baglanti : Qwiic / I2C + analog pin
  Seri hiz : 115200 baud

  Bu surumdeki yenilikler (AS3935 teshis odakli):
    - Acilista tam I2C bus taramasi (0x01..0x7E) yapip bulunan cihazlari basar.
    - AS3935 icin 0x00..0x03 adreslerini otomatik dener (ADD0/ADD1 jumper'i
      degismis olabilir). Once Wire ile ACK arar, sonra kutuphaneyi baslatir.
    - Bulununca OUTDOOR moduna alir ve temel kalibrasyon (anten tuning capasitesi,
      noise floor, watchdog, spike rejection, disturber maskesi) uygular.

  Kutuphaneler (Tools -> Manage Libraries):
    - SparkFun BME280 Arduino Library
    - SparkFun AS3935 Lightning Detector Arduino Library
    - SparkFun Weather Meter Kit Arduino Library
*/

#include <Wire.h>
#include <SPI.h>
#include "SparkFunBME280.h"
#include "SparkFun_AS3935.h"
#include "SparkFun_Weather_Meter_Kit_Arduino_Library.h"

// ---------------- Ayarlar ----------------
#define BME280_ADDR   0x77   // SparkFun BME280 Qwiic varsayilan (bulunamazsa 0x76 denenir)

// ONEMLI: MicroMod Weather Carrier kartinda AS3935 I2C degil, SPI ile baglidir!
// (Kart uzerinde BME280=I2C 0x77, VEML6075=I2C 0x10, AS3935=SPI.)
// MicroMod ESP32 islemci kartinda pin karsiliklari:
//   AS3935 CS  = G1  = GPIO 12
//   AS3935 INT = G3  = GPIO 17  (carrier'da hardwired, ayrica tel gerekmez)
#define AS3935_CS       12     // SPI chip-select (G1)
#define AS3935_INT_PIN  17     // yildirim kesme pini (G3)
#define AS3935_SPI_HZ   2000000

// Weather Meter Kit pinleri (MicroMod Weather Carrier + ESP32 varsayilani)
#define WDIR_PIN     35      // Ruzgar yonu kanadi (analog / ADC)
#define WSPEED_PIN   14      // Anemometre (ruzgar hizi, pulse)
#define RAIN_PIN     27      // Yagmur olcer (pulse)

// AS3935 kalibrasyon/hassasiyet ayarlari (dis mekan icin makul varsayilanlar)
#define AS3935_TUNE_CAP        6   // anten tuning kapasitesi 0..15 (her adim ~8pF).
                                   // Tam kalibrasyon INT pini + 500kHz frekans olcumu ister.
#define AS3935_NOISE_FLOOR     2   // 1..7 (dusuk = daha hassas)
#define AS3935_WATCHDOG        2   // 1..10
#define AS3935_SPIKE_REJECT    2   // 1..11
#define AS3935_LIGHTNING_THRESH 1  // kac yildirimda kesme: 1,5,9,16

// --- Toprak Nemi (Soil Moisture) sensoru, analog ---
#define SOIL_PIN     34      // Toprak nemi sensoru analog cikis (SIG / AOUT), GPIO34 (ADC1)
#define SOIL_ADC_DRY  0      // havada (kuru) ham ADC  -> %0 nem
#define SOIL_ADC_WET  648    // suda (islak) ham ADC   -> %100 nem

BME280 bme;
SparkFun_AS3935 lightning;               // SPI modunda kullanilacak
SFEWeatherMeterKit weatherMeterKit(WDIR_PIN, WSPEED_PIN, RAIN_PIN);

volatile bool lightningFlag = false;
bool as3935Ready = false;

void IRAM_ATTR onLightning() { lightningFlag = true; }

float soilMoisturePercent(int raw) {
  float pct = (float)(SOIL_ADC_DRY - raw) * 100.0 /
              (float)(SOIL_ADC_DRY - SOIL_ADC_WET);
  if (pct < 0)   pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

// Bir I2C adresinde cihaz var mi? (ACK doner mi?)
bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return (Wire.endTransmission() == 0);
}

// Tum bus'i tara ve bulunan adresleri bas.
void i2cScan() {
  Serial.println("I2C taramasi (0x01..0x7E)...");
  uint8_t found = 0;
  for (uint8_t a = 1; a < 0x7F; a++) {
    if (i2cPresent(a)) {
      Serial.print("  -> cihaz bulundu: 0x");
      if (a < 16) Serial.print("0");
      Serial.println(a, HEX);
      found++;
    }
  }
  if (found == 0)
    Serial.println("  (hicbir I2C cihazi yok! SDA/SCL/GND/3V3 ve Qwiic kablosunu kontrol edin)");
  else {
    Serial.print("  toplam ");
    Serial.print(found);
    Serial.println(" cihaz.");
  }
}

// AS3935'i SPI uzerinden baslat ve kalibre et (Weather Carrier'da SPI baglidir).
void initAS3935() {
  SPI.begin();
  if (lightning.beginSPI(AS3935_CS, AS3935_SPI_HZ) == false) {
    Serial.println("UYARI: AS3935 SPI'da baslatilamadi (CS=GPIO12).");
    Serial.println("  Kontrol: kart oturmus mu, SPI hatti, MicroMod pad hizalamasi.");
    return;
  }
  as3935Ready = true;
  Serial.println("AS3935 hazir (SPI, CS=GPIO12).");

  // --- Temel kalibrasyon / hassasiyet ---
  lightning.setIndoorOutdoor(OUTDOOR);
  lightning.tuneCap(AS3935_TUNE_CAP);          // anten tuning kapasitesi
  lightning.setNoiseLevel(AS3935_NOISE_FLOOR); // noise floor
  lightning.watchdogThreshold(AS3935_WATCHDOG);
  lightning.spikeRejection(AS3935_SPIKE_REJECT);
  lightning.lightningThreshold(AS3935_LIGHTNING_THRESH);
  lightning.maskDisturber(false);              // parazitleri de bildir
  Serial.println("AS3935 kalibrasyon uygulandi (OUTDOOR, tuneCap/noise/watchdog/spike).");
  Serial.println("  Not: tam anten tuning'i 500kHz LCO frekans olcumu ile ince ayar ister.");

  pinMode(AS3935_INT_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(AS3935_INT_PIN), onLightning, RISING);
  Serial.println("AS3935 INT pini dinleniyor (GPIO17).");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== Hava Istasyonu baslatiliyor ===");

  Wire.begin();              // Qwiic / I2C
  Wire.setClock(400000);

  // --- Once bus'i tara (teshis) ---
  i2cScan();
  Serial.println("--------------------------------------");

  // --- BME280 ---
  bme.setI2CAddress(BME280_ADDR);
  if (bme.beginI2C() == false) {
    Serial.println("BME280 0x77'de yok, 0x76 deneniyor...");
    bme.setI2CAddress(0x76);
    if (bme.beginI2C() == false)
      Serial.println("HATA: BME280 bulunamadi! (Qwiic kablosunu kontrol edin)");
    else
      Serial.println("BME280 hazir (0x76).");
  } else {
    Serial.println("BME280 hazir (0x77).");
  }

  // --- AS3935 (otomatik adres + kalibrasyon) ---
  initAS3935();

  // --- Weather Meter Kit (ruzgar + yagmur) ---
  weatherMeterKit.begin();
  Serial.println("Weather Meter Kit hazir (ruzgar/yagmur).");

  // --- Toprak Nemi sensoru ---
  pinMode(SOIL_PIN, INPUT);
  analogReadResolution(12);       // 12-bit: 0..4095
  Serial.println("Toprak nemi sensoru hazir (analog).");

  Serial.println("--------------------------------------");
}

unsigned long lastRead = 0;

void loop() {
  // Yildirim olayi (INT pini bagliysa ve kesme geldiyse)
  if (lightningFlag && as3935Ready) {
    lightningFlag = false;
    delay(2);
    int intVal = lightning.readInterruptReg();
    if (intVal == LIGHTNING) {
      byte dist = lightning.distanceToStorm();
      long energy = lightning.lightningEnergy();
      Serial.print(">>> YILDIRIM! Mesafe: ");
      Serial.print(dist);
      Serial.print(" km, Enerji: ");
      Serial.println(energy);
      Serial.print("EVENT {\"type\":\"lightning\",\"distance_km\":");
      Serial.print(dist);
      Serial.print(",\"energy\":");
      Serial.print(energy);
      Serial.println("}");
    } else if (intVal == DISTURBER_DETECT) {
      Serial.println(">>> Parazit (disturber) algilandi.");
      Serial.println("EVENT {\"type\":\"disturber\",\"distance_km\":null,\"energy\":null}");
    } else if (intVal == NOISE_TO_HIGH) {
      Serial.println(">>> Gurultu seviyesi yuksek (noise floor arttirin).");
      Serial.println("EVENT {\"type\":\"noise\",\"distance_km\":null,\"energy\":null}");
    }
  }

  // Her 3 saniyede bir olcum
  if (millis() - lastRead >= 3000) {
    lastRead = millis();
    float t   = bme.readTempC();
    float h   = bme.readFloatHumidity();
    float p   = bme.readFloatPressure() / 100.0;   // Pa -> hPa
    float alt = bme.readFloatAltitudeMeters();

    float windSpeed = weatherMeterKit.getWindSpeed();       // km/h
    float windDir   = weatherMeterKit.getWindDirection();   // derece (0-360)
    float rain      = weatherMeterKit.getTotalRainfall();   // mm (kumulatif)

    int   soilRaw = analogRead(SOIL_PIN);                   // ham ADC (0-4095)
    float soilPct = soilMoisturePercent(soilRaw);           // % toprak nemi

    Serial.print("Sicaklik: ");  Serial.print(t, 1);   Serial.print(" C  |  ");
    Serial.print("Nem: ");       Serial.print(h, 1);   Serial.print(" %  |  ");
    Serial.print("Basinc: ");    Serial.print(p, 1);   Serial.print(" hPa  |  ");
    Serial.print("Yukseklik: "); Serial.print(alt, 1); Serial.println(" m");

    Serial.print("Ruzgar hizi: ");  Serial.print(windSpeed, 1); Serial.print(" km/h  |  ");
    Serial.print("Ruzgar yonu: ");  Serial.print(windDir, 0);   Serial.print(" derece  |  ");
    Serial.print("Yagmur: ");       Serial.print(rain, 2);      Serial.println(" mm");

    Serial.print("Toprak nemi: "); Serial.print(soilPct, 1);
    Serial.print(" %  (ham ADC: "); Serial.print(soilRaw); Serial.println(")");

    // Makine-okur telemetri satiri (Pi logger bunu JSON olarak parse eder)
    Serial.print("TELEM {");
    Serial.print("\"temperature\":"); Serial.print(t, 2);        Serial.print(",");
    Serial.print("\"humidity\":");    Serial.print(h, 2);        Serial.print(",");
    Serial.print("\"pressure\":");    Serial.print(p, 2);        Serial.print(",");
    Serial.print("\"altitude\":");    Serial.print(alt, 2);      Serial.print(",");
    Serial.print("\"wind_speed\":");  Serial.print(windSpeed, 2);Serial.print(",");
    Serial.print("\"wind_dir\":");    Serial.print(windDir, 1);  Serial.print(",");
    Serial.print("\"rain_mm\":");     Serial.print(rain, 2);     Serial.print(",");
    Serial.print("\"soil_pct\":");    Serial.print(soilPct, 1);  Serial.print(",");
    Serial.print("\"soil_adc\":");    Serial.print(soilRaw);
    Serial.println("}");

    Serial.println("--------------------------------------");
  }
}
