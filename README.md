# WeatherStation

Arduino sketch for an ESP32 weather station on the SparkFun MicroMod Weather Carrier.
It reads every sensor on the board every 3 s and prints both a human-readable line and a
machine-readable JSON line over serial, ready for a gateway or uploader to pick up.

## Sensors

| Sensor | Measures | Bus / pin |
|---|---|---|
| BME280 | temperature, humidity, pressure, altitude | I2C `0x77` (falls back to `0x76`) |
| AS3935 | lightning strikes, distance and energy | SPI, CS = GPIO 12, INT = GPIO 17 |
| SparkFun Weather Meter Kit | wind speed, wind direction, rain | GPIO 14 / 35 / 27 |
| Capacitive soil moisture probe | soil moisture % | ADC, GPIO 34 |

At boot the sketch scans the I2C bus and reports every device it finds. It then puts the
AS3935 in outdoor mode and applies tuning-cap, noise-floor, watchdog and spike-rejection settings.

## Serial output (115200 baud)

Telemetry, every 3 s:

```
TELEM {"temperature":21.53,"humidity":48.20,"pressure":1012.40,"altitude":98.10,"wind_speed":3.20,"wind_dir":135.0,"rain_mm":0.00,"soil_pct":41.3,"soil_adc":268}
```

Lightning events, interrupt-driven:

```
EVENT {"type":"lightning","distance_km":12,"energy":53412}
EVENT {"type":"disturber","distance_km":null,"energy":null}
```

## Build

Arduino IDE or `arduino-cli` with the Espressif ESP32 core. Libraries:

- SparkFun BME280 Arduino Library
- SparkFun AS3935 Lightning Detector Arduino Library
- SparkFun Weather Meter Kit Arduino Library

Soil moisture calibration (`SOIL_ADC_DRY` / `SOIL_ADC_WET`) and the AS3935 sensitivity
settings are `#define`s at the top of `WeatherStation.ino`.

## Related

- [weather_lora_link](https://github.com/zainnmiann/weather_lora_link): carries these readings from the station to a gateway over LoRa.
