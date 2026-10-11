# Zigbee Doorbell

Zigbee-Türklingel-Anlage mit zwei ESP32-C6-Geräten. Der Empfänger spielt einen
Klingelton ab, der Sender ist ein batteriebetriebener Taster mit Deep Sleep.

## Struktur

```
receiver/ZigbeeBellV2/          Empfänger (MAX98357A-Lautsprecher)
├── ZigbeeBellV2.ino            Hauptsketch
├── data/bell.wav               Eingabe fuer das LittleFS-Image
└── audio/                      Quelldateien (MP3) als Basis für bell.wav

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

Der Sender misst die Akkuspannung an GPIO 0 und meldet zwei Werte:

- **Batterie-Prozent** aus der gemessenen Spannung im Fenster 3,2 – 3,6 V
- **Batterie-Spannung** in 100-mV-Einheiten, wie es die Cluster-API erwartet
  (34 entspricht 3,4 V)

Die Messung nutzt `analogReadMilliVolts()` mit 11 dB Dämpfung statt eines
rohen `analogRead()` mit fest verdrahteter Referenzspannung, und mittelt
16 Messungen.

Die Spannung am ADC-Pin wird unverändert als Akkuspannung gewertet — ein
100k/100k-Teiler ist **nicht** eingerechnet. Grund: die Messung zeigt die
volle Spannung am Pin, eine Rückrechnung auf das Doppelte ergäbe Werte
über 6 V. Ob am Board tatsächlich ein Teiler verbaut ist, ist offen; falls
ja, muss `readBattery()` den Faktor 2 wieder einführen.

Der Prozentwert ist eine lineare Interpolation über 3,2 – 3,6 V. Das
Fenster ist nur 400 mV breit, dadurch ist die Anzeige grob: ±50 mV
Messfehler verschieben sie um rund 12 Prozentpunkte. Für eine Li-Ion-Zelle
ist zudem 3,6 V nicht der Ladezustand 100 %, sondern eher 60 – 75 %; wer
eine brauchbare Prozentangabe braucht, braucht eine stückweise
Entladekurve und ein Fenster bis 4,2 V.

Der Binary Input wird nach 5 Sekunden wieder zurückgesetzt, damit der
Coordinator ein Ereignis sieht und keinen Dauerzustand. Vor dem Deep Sleep
wartet der Sender 500 ms (`REPORT_SETTLE_MS`), weil `reportBinaryInput()`
den Report nur in den Sende-Puffer legt und sofort zurückkehrt — ohne die
Pause schläft das Gerät weg, bevor das „false" den Coordinator erreicht.

## Bauen

Vor dem Kompilieren **drei Einstellungen unter Tools prüfen**. Sie
stehen in keiner Projektdatei, sondern nur in der IDE und fallen beim
Wechsel des Boards oder des Partition Scheme auf ihre Defaults zurück —
ein häufiger Stolperstein:

| Einstellung | Wert | Warum |
|---|---|---|
| **Board** | ESP32-C6 Dev Module | betrifft beide |
| **Zigbee mode** | `End Device` | Sketch startet sonst gar nicht (`#error`) |
| **Partition Scheme** | `Zigbee 4MB with spiffs` bzw. `Zigbee 8MB with spiffs` | ohne `zb_storage` bricht der Zigbee-Stack ab |
| **USB CDC On Boot** | `Enabled` | sonst läuft `Serial` über UART0 statt USB |

**Partition Scheme** richtet sich nach der Flash-Größe des Moduls.
Das Board meldet seine Größe unter **Tools → Partition Scheme** nicht —
im Zweifel mit `esptool --chip esp32c6 --port /dev/ttyACM0 flash-id`
nachsehen (Zeile `Detected flash size`). Die 4-MB-Variante ist bei
C6-Modulen der Regelfall.

> `upload.maximum_size` liegt bei `Zigbee 4MB with spiffs` nur bei
> 1.310.720 Byte für die App. Bei größeren Libs wird der Build daran scheitern.

**USB CDC On Boot** ist besonders tückisch, weil der ROM-Bootloader
seine Ausgabe unabhängig davon über den USB-Port schickt. Man sieht
also weiterhin das `ESP-ROM:esp32c6-...`-Banner und Assert-Meldungen
des Stacks, aber **keine einzige Zeile aus `setup()`** — das Gerät
sieht aus, als hinge es, obwohl es normal arbeitet. Auf dem ESP32-C6
ist `Serial` ohne diese Einstellung UART0 auf GPIO16/GPIO17, an dem
bei einem reinen USB-Anschluss nichts anliegt.

Bei aktivierter USB-CDC erscheint die Ausgabe erst, sobald ein Host
den Port öffnet — der Serial Monitor muss also laufen. Vor dem
Flashvorgang schließen, danach neu öffnen, weil sich der Port kurz
trennt. Gilt für **beide** Geräte: auch der Sender gibt seine
Batteriewerte und die Wakeup-Ursache über `Serial` aus.

### Dateisystem des Empfängers

Die Erweiterung *arduino-littlefs-upload* baut das Image beim
Hochladen selbst aus `data/` und übernimmt Größe und Offset aus dem
gewählten Partition Scheme. **Tools → Upload LittleFS to
Pico/ESP8266/ESP32** reicht also — ein fertiges Image muss nicht
mit ins Repo. Derselben Menüpunkte gibt es auch als
**Build LittleFS image in sketch directory**, um das Image nur
zu erzeugen, ohne es zu flashen; es landet dann als
`mklittlefs.bin` im Sketch-Ordner.

Wird es manuell eingespielt, muss die Größe **exakt** zur
`spiffs`-Partition des Schemas passen:

```bash
mklittlefs -c data -s 0x15B000 -b 4096 -p 256 bell.fs
```

| Partition Scheme | LittleFS-Offset | Image-Größe |
|------------------|-----------------|-------------|
| `zigbee` (4 MB)  | `0x290000`      | `0x15B000`  |
| `zigbee_8MB`     | `0x690000`      | `0x15B000`  |

**Nach einem Wechsel des Partition Scheme das Dateisystem
neu einspielen.** Ein bereits geschriebenes Image passt zur neuen
Partitionsgröße nicht mehr und lässt den Mount mit einem Assert in
`lfs_fs_grow_` abbrechen. Ein vorheriger „ESP32 Erase Flash“ ist
der saubere, aber nicht der einzige Weg — er löscht auch NVS
(Gain und Klingeldauer fallen auf die Defaults) und das
Zigbee-Pairing, das Gerät muss also neu angelernt werden.

## Fehlersuche

| Meldung | Ursache |
|---------|---------|
| `ZB_ESP_NVRAM: Failed to find zb_storage partition` | Falsches Partition Scheme — `zb_storage` fehlt |
| `assert failed: lfs_fs_grow_` | LittleFS-Image passt nicht zur Partition; erase flash und Image neu bauen |
| `/bell.wav nicht gefunden!` | Image fehlt, wurde am falschen Offset eingespielt oder nicht neu gebaut |
| `Zigbee failed to start!` + Reboot | Siehe Assert-Meldung darüber im Log |
| ROM-Boot-Banner sichtbar, aber sonst **keine** Ausgabe | `USB CDC On Boot` steht auf Disabled — `Serial` läuft über UART0 statt USB |

## Offene Punkte

- **Batterie-Prozent ist ungenau.** Lineare Interpolation über 3,2 – 3,6 V,
  siehe oben. Für eine brauchbare Anzeige braucht es eine stückweise
  Entladekurve und ein Fenster bis 4,2 V.
- **Spannungsteiler unklar.** Der Faktor 2 ist entfernt, weil die Messung
  die volle Spannung am Pin zeigt. Ob am Board ein 100k/100k-Teiler
  verbaut ist, wurde nicht verifiziert.
- **Die ADC-Reichweite begrenzt das Messfenster.** Mit 11 dB Dämpfung
  liegt der messbare Bereich beim C6 bei rund 3,1 V. Ohne Teiler ist
  darüber liegende Spannung (z. B. eine volle Li-Ion-Zelle mit 4,2 V)
  nicht sauber erfassbar.
- **`REPORT_SETTLE_MS` ist ein heuristischer Wert.** Die Bibliothek bietet
  keine Möglichkeit, auf das tatsächliche Absenden des Reports zu warten.