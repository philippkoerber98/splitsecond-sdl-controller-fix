# Split/Second SDL Controller Fix

Version 1.4 für die Windows-PC-Fassung von Split/Second. SDL übernimmt Controller-Eingaben und die originalen Vibrationssignale des Spiels. Kein externes Mapping-Programm und keine Steam-Konfiguration erforderlich.

## Installation

[Fertige v1.4-ZIP herunterladen](dist/SplitSecond-Controller-Fix-v1.4.zip). Spiel schließen und den ZIP-Inhalt direkt neben `SplitSecond.exe` entpacken. Die drei DLLs müssen im Spielordner liegen. Anschließend das Spiel normal starten. Eine vorhandene `dinput8.dll` einer anderen Mod vorher sichern; deren Kompatibilität ist separat zu prüfen.

Die Spiel-EXE wird nicht verändert und ist nicht Bestandteil dieses Repositorys. Zum Entfernen bei originaler EXE die drei mitgelieferten DLLs löschen. Weitere Hinweise: [LIESMICH.txt](LIESMICH.txt).

## Funktionen

- Controller-Steuerung in Menü und Rennen, zwei unabhängige Controller im lokalen Splitscreen.
- Automatischer Wechsel zwischen Xbox-Symbolen und Tastaturanzeige anhand der zuletzt verwendeten Eingabe.
- Xbox-Tastennamen im Steuerungsmenü, einschließlich umbelegter Eingaben.
- LT und RT unabhängig und gleichzeitig analog; getrennte aktuelle und vorherige Eingabepuffer je Controller.
- Originale Vibrationswerte für beide Motoren unverändert über SDL; keine künstlichen Effekte.
- Neutrale Eingaben beim Trennen und Wiederverbinden während derselben Sitzung.

Bei 8BitDo nach Möglichkeit den Windows-/XInput-Modus verwenden. Identische Controller ohne Seriennummer können nach gleichzeitigem Trennen physisch nicht sicher auseinandergehalten werden.

## Kompatibilität und Nachweise

Getesteter SHA256 der originalen PC-EXE:

```text
D11FC7F48DD504BDEFB16B548980BD41101F6C1A23D29C03487A81D4DE42173E
```

Spielabhängige Hooks prüfen die erwarteten Instruktionen. Andere EXE-Versionen sind nicht pauschal unterstützt; bei abweichendem Aufbau kann insbesondere die kombinierte DirectInput-Triggerachse weiterhin gelten.

Der Integrationstest prüft 10.201 gleichzeitige Triggerkombinationen pro Controller durch die originalen PC-Achsen- und Aktionsroutinen, außerdem Umbelegung, Eingabepuffer und Wiederverbinden. Ein Test im laufenden Splitscreen protokolliert unterschiedliche gleichzeitige Intensitäten für beide Spieler. Adaptertests prüfen unter anderem getrennte Motorwerte und SDL-Ausgabefehler.

Diese Prüfungen verwendeten virtuelle SDL-Controller. Echte Xbox-One-/8BitDo-Hardware, Bluetooth/USB und physische Vibrationsmotoren sind dadurch nicht abschließend verifiziert. Kein direkter Vergleich mit der Xbox-360-Fassung und kein quantitatives Driftphysikexperiment. Die originale PC-Fahrphysik bleibt erhalten.

Details und Messwerte: [Prüfbericht](tests/PRUEFBERICHT.txt), [Integrationstest-Ergebnis](tests/independent-triggers-result.txt), [Live-Messung](tests/independent-triggers-live.csv).

## Selbst bauen

Visual Studio 2022 **x86 Native Tools** und das offizielle [SDL3-devel-3.4.20-VC](https://github.com/libsdl-org/SDL/releases/tag/release-3.4.20) verwenden. Im Repository-Verzeichnis, mit entpacktem SDL-Paket daneben:

```bat
cl /LD /std:c++17 /EHsc /O2 /MT /D_CRT_SECURE_NO_WARNINGS /I SDL3-3.4.20\include sspad.cpp prompts.cpp triggers.cpp /link /DEF:sspad.def /OUT:sspad.dll SDL3-3.4.20\lib\x86\SDL3.lib user32.lib
copy sspad.dll dinput8.dll
```

Beide erzeugten DLLs und `SDL3-3.4.20\lib\x86\SDL3.dll` in den Spielordner kopieren. Die Test-Buildbefehle stehen im Prüfbericht. Der Integrationstest benötigt eine eigene kompatible Spiel-EXE; diese wird nicht mitgeliefert.

## Herkunft und Lizenzen

Controller-Adapter auf Grundlage von [Back on Air](https://github.com/xenophobentx/back-on-air), Commit `57d17d88dc02e56ebcc6686efc6c20226d9cc5f1`, unter [MIT-Lizenz](LICENSE.txt). Die ursprünglichen Urheberhinweise bleiben erhalten. Diese Fassung ergänzt unter anderem Geräteanzeige, Xbox-Bezeichnungen, Rumble-Weiterleitung und getrennte analoge Trigger im Spielpfad.

SDL 3.4.20 steht unter der [zlib-Lizenz](SDL-LICENSE.txt). Split/Second und zugehörige Marken gehören ihren jeweiligen Rechteinhabern. Dieses Projekt ist eine inoffizielle Modifikation.
