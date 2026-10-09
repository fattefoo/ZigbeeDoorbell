# Zigbee Doorbell

Zigbee-Türklingel-Anlage mit zwei ESP32-C6-Geräten. Der Empfänger spielt einen
Klingelton ab, der Sender ist ein batteriebetriebener Taster mit Deep Sleep.

## Struktur

```
receiver/ZigbeeBellV2/          Empfänger (MAX98357A-Lautsprecher)
├── ZigbeeBellV2.ino            Hauptsketch
├── data/bell.wav               Datei für das LittleFS-Partition
├── audio/                      Quelldateien (MP3) als Basis für bell.wav
└── tools/mklittlefs.bin        LittleFS-Image-Baukasten

transmitter/ZigbeeDoorbellSensorV2/   Sender (Optokoppler, Deep Sleep)
└── ZigbeeDoorbellSensorV2.ino

archive/                       Alte Versionen, nur Referenz, NICHT flashen
├── ZigbeeBell_v1/
└── ZigbeeDoorbellSensor_v1/
```

Arduino verlangt, dass der Ordnername dem `.ino`-Namen entspricht — deshalb
liegen die Sketches in eigenen Unterordnern. Der Projektordner selbst ist kein
gültiger Sketch.

## Cluster

Der Empfänger registriert drei Endpoints:

| EP | Typ | Beschreibung | Wertebereich |
|----|-----|--------------|--------------|
| 1 | Binary Output | Klingeln (Trigger) | an/aus |
| 1 | Binary Input | Klingelt (Zustand) | an/aus |
| 2 | Analog Output | Lautstärke | 0.0 – 4.0 |
| 3 | Analog Output | Klingeldauer | 1000 – 7500 ms |

Gain und Dauer werden in den NVS-Partition unter dem Namen `Prefs` gespeichert
und beim Boot wiederhergestellt.

## Taster

Beide Geräte nutzen die gleiche Belegung: kurzer Druck löst aus, langer Druck
(>3 s) macht den Zigbee Factory Reset.

- **Empfänger**: eigener FreeRTOS-Task, damit die Wiedergabe beim Entprellen
  nicht blockiert wird.
- **Sender**: entprellt direkt in `loop()`, da das Gerät ohnehin regelmäßig in
  den Deep Sleep geht.

## Batterieüberwachung (Sender)

Der Sender misst die Akkuspannung über einen 100k/100k-Spannungsteiler an
GPIO 0 und meldet zwei Werte:

- **Batterie-Prozent** aus der zurückgerechneten Akkuspannung (3,2 – 3,6 V)
- **Batterie-Spannung** in 100-mV-Einheiten, wie es die Cluster-API erwartet
  (34 entspricht 3,4 V)

Die Messung nutzt `analogReadMilliVolts()` mit 11 dB Dämpfung statt eines
rohen `analogRead()` mit fest verdrahteter Referenzspannung.

Der Binary Input wird nach 5 Sekunden wieder zurückgesetzt, damit der
Coordinator ein Ereignis sieht und keinen Dauerzustand.

## Bauen

Tools → Zigbee mode → **End Device**, dann für den ESP32-C6 kompilieren.

Die Klingel-Datei wird mit `tools/mklittlefs.bin` in ein LittleFS-Image
verwandelt und auf das Dateisystem des Empfängers geflasht.

## Bekannte offene Punkte

- `SetGain` wird beim Empfänger mit Werten bis 4.0 aufgerufen. Ob die
  verwendete ESP8266Audio-Version diesen Bereich sauber abbildet, ist bisher
  nur auf dem Gerät verifizierbar.
- Der GPIO-Wakeup erwartet, dass der Optokoppler bei einem Klingelsignal auf
  HIGH zieht (`ESP_GPIO_WAKEUP_GPIO_HIGH`). Falls der Ausgang invertiert ist,
  wacht der Sender nicht auf. Ohne Schaltplan nicht prüfbar.
- Beide Sketches sind bisher nur **kompiliert**, nicht auf Hardware getestet.
- Das Projekt ist kein Git-Repository. Vor größeren Änderungen wäre ein
  `git init` sinnvoll.