# StageRack — contesto per Claude

Host di plugin per audio LIVE stile Waves SuperRack Performer (+ LiveProfessor/Ableton). C++20 / JUCE 9.0.5 (submodule `libs/JUCE`, AGPLv3; CLAP in `libs/clap`). App universale arm64+x86_64, macOS 11+.
**Dettagli, misure e cronaca di ogni funzione: `docs/DEV-NOTES.md` (cercaci con grep prima di ri-esplorare o ripetere esperimenti).** Manuale utente `docs/MANUALE.md`, misure `docs/MISURE.md`, novità `CHANGELOG.md`.

## Utente
- Fonico live, italiano: rispondi in italiano, conciso; apprezza il "perché" e le misure concrete, verifica a vista.
- Mac M4 Pro: 8 P-core (CPU 4-11) + 4 E-core (0-3). Scheda: Dante Virtual Soundcard (apertura ~20 s in coreaudiod, non è un bug nostro) o "Altoparlanti MacBook Pro".
- Priorità: **zero latenza aggiunta** (ASYNC rifiutato e rimosso), buffer 32 (anche 16), CPU leggera, multicore.
- Plugin chiave: Alpha Labs De-Feedback (VST3/AU), EverClean (Everworks). 578 plugin scansionati.
- Può avere l'app aperta mentre lavori: prima di chiuderla o toccare `~/Library/Application Support/StageRack/` controlla; per prove usa `STAGERACK_DATA_DIR` con una copia dei dati.

## Build / run / test
```bash
cmake --build build                     # Ninja Release; riconfigura: cmake -S . -B build -G Ninja
EXE=build/StageRack_artefacts/Release/StageRack.app/Contents/MacOS/StageRack
$EXE --selftest                         # + --edit-selftest, --undo-selftest, --device-guard-test
arch -x86_64 $EXE --selftest            # slice Intel
$EXE --session-check <file.stagerack>   # compatibilità sessione (4-5 sessioni in ~/Documents)
$EXE --bench-device VST3 "/Library/Audio/Plug-Ins/VST3/Defeedback.vst3" <racks> <buffer> Altoparlanti --multicore-only [--per-rack N] [--input wav sec] [--buses N|--master-bus] [--noalign] [--nowake]
scripts/package.sh                      # dist/ firmata ad-hoc + zip
scripts/release.sh                      # package + test + tag v<versione> + release GitHub (note = sezione CHANGELOG)
```
- `--bench-device` (CoreAudio reale, workgroup vero) è **l'unico benchmark affidabile**; `--bench` offline è pessimista.
- Altri strumenti CLI (vedi `Source/Main.cpp`): `--plugin-probe`, `--watch-test`, `--spread-test`, `--handover-test`, `--automix-probe`, `--soak`, `--sandbox-test`, `--stress-lifecycle`, `--midi-loop-test`, `--param-probe`, `--tempo-probe`, `--check-update [--download] [--fake-version]`, `--whats-new`, render PNG senza device: `--render-racks <dir> [--routing|--midi|--view n]`, `--render-session`, `--render-settings`, `--render-browser`, `--render-changelog`. `STAGERACK_UI_BENCH=<n>` misura l'interfaccia con n rack.
- Flusso: build dopo ogni modifica (`cmake --build build 2>&1 | grep -E "error" -A3`), selftest, bench-device per le prestazioni, verifica a vista. Aggiorna `docs/MANUALE.md` per le funzioni nuove, `CHANGELOG.md` (italiano, `## x.y.z-beta — data` in cima) per le release, e qui solo ciò che serve a lavorare.
- Repo pubblico github.com/Bio00/StageRack, bundle id `com.bio.stagerack`. Ultima release v0.11.3-beta. Versione = `project VERSION` + `STAGERACK_VERSION_LABEL` in CMake, deve coincidere col tag. Autore commit = noreply GitHub, mai la Gmail.
- Dati app: `~/Library/Application Support/StageRack/` (`StageRack.settings` XML, `plugins.xml`, `latency.xml`, `dropouts.log`, `Autosave.stagerack`). Sessione di test `~/Documents/StageRack-Test.stagerack`; sessione grande dell'utente `~/Documents/ZSP BIO.stagerack` (20 rack).

## Architettura (Source/)
- `Engine/AudioEngine` — callback: rack a **stadi** (catene/bus = stadio successivo, stesso callback), parallelo adattivo (`WorkerPool`: worker TC nel workgroup CoreAudio, P-core − 1), grafo calcolato senza lock (`planGraph`) e scambiato sotto `racksLock` (`applyGraphLocked`); automix, solo, master mute, hop watch (`watchHops`), spread (`alignAllRacks/alignNewPlugin`, `alignmentOrder`, `choosePhase`), `armWakeAssist`, `hopFirstOrder`, compensazione latenza (gruppi A-H), cache canali/latenza device (mai query HAL in paint).
- `Engine/Rack` — 8 slot in serie, I/O, trim/gain smussati, meter, delay di compensazione, send/bus/uscite extra, automix detector, handover (gemello + crossfade), hop watch per slot (`heavyAt`, `hopWaitMs`, `wakeNeed`).
- `Engine/HopAligner`, `CoreWakeAssist`, `Session` (+`LiveRespreader`, undo a snapshot con token, scene, cartelle, istanze parcheggiate + `tidyParkedPlugins`), `Autosave`, `DeviceGuard`, `TempoEngine/TempoOutput/OscInput`, `MidiControl/MidiMap/MidiProfiles`, `DropoutMonitor/DropoutLog`, `SystemHealth.mm`, `RtLock` (attesa limitata), `RtGuard` (rileva malloc nel nostro codice audio), `CoreAudioProbe` (prima chiamata CoreAudio in un processo figlio), `Features.h` (funzioni spegnibili = nascoste e inattive).
- `Plugins/` — `PluginManager` (scansione out-of-process, cache latenze, `applyInsertDefaults`), `CLAPPluginFormat.mm` (host CLAP nostro), `Sandbox/` (plugin isolati sperimentali), `ParamTwins.h`, `AuWorkgroup.mm`.
- `Update/` — `Updater` (release GitHub, verifica, installer script), `Changelog`.
- `UI/` — `MainComponent` (layout, comandi/menu, timer), `RackComponent.cpp` (rack, slot, ParamRow), `TopBar` (blocchi personalizzabili), `SendsPanel`, `PluginBrowser`, `PluginManagerWindow` (SETTINGS a sezioni), `AudioDevicePanel` (Apply), `TempoWindow`, `MidiWindow/MidiMapOverlay`, `AutomixerWindow`, `EditController` (selezione, ⌥/gruppi), `Theme.h`, `ParamSlider.h`, `ValueEntry.h`, `AppMenu.mm`.
- Test: `Source/*Test.h` (in `--selftest`: Routing, Tempo, Midi; Edit, Undo, DeviceGuard separati), `Bench.h`.

## Scoperte misurate (non ripetere; dettagli in DEV-NOTES)
- **De-Feedback**: hop rete neurale ogni 192 campioni, latenza 0; quasi tutto il calcolo nel callback dell'hop (0.17-0.45 ms); il VST3 aspetta un thread helper, l'AU no. **EverClean**: hop 192, tutto sul thread audio (~0.3 ms) → @16 impossibile; `Active` Off all'inserimento (lo accende `applyInsertDefaults`); contatore frame si ferma se Off > 40 ms.
- Hop allineati = picchi → **spread** (HopAligner, silenzio prima di andare live; `prepareToPlay` azzera la fase → riallineare dopo ogni prepare). Dal vivo: hop watch + `LiveRespreader` (handover senza click). Rack in serie: fasi diverse (costo 1000), master bus allineato per primo.
- Core in idle profondo: svegliare un core costa ~0.25 ms → **CoreWakeAssist** solo per tipi di plugin che aspettano davvero (`wakeNeed`, misurato al primo hop dopo una pausa).
- Il workgroup CoreAudio è decisivo (CLPC alza il clock). Scartati (nessun effetto o peggio): spin dei worker, promozione QoS dei thread del plugin, ballast, AMX warm-up, `VECLIB_MAXIMUM_THREADS=1`, offload dei rack, ASYNC, SDK VST3 diretto (involucro JUCE ~3 µs).
- Risultati: 1-16 DF @32 = 0 persi; 12 DF @16 = 0 persi. Senza spread 8 DF → ~1700 persi.
- Crash/blocchi di De-Feedback (heap corruption in creazione/distruzione, deadlock in dispose) sono **del plugin**: mitigati con parcheggio istanze, autosave, timeout di uscita 5 s. Non disassemblare plugin commerciali.

## Trappole
- **Uscita**: `shutdown` → `prepareToQuit` (chiude finestre, salva, chiude device, distrugge plugin) → `_Exit(0)`. Mai lasciar girare i distruttori JUCE (ricreano la finestra e la ridipingono con sessione distrutta). Heap danneggiati da plugin terzi esplodono al primo malloc successivo.
- Distruggere istanze VST3 fuori dal message loop crasha: nei test CLI tenerle vive e uscire con `_Exit`. Il gemello del re-spread non va mai distrutto subito: `retirePlugin`.
- Callback CoreAudio di JUCE: canali **attivi compattati** → `setChannelMaps`. `outputChannel = -1` va saltato. `getXRunCount` non è affidabile.
- `juce::File::replaceWithText` scrive CRLF: per script bash passare `"\n"`.
- `CMAKE_OSX_DEPLOYMENT_TARGET/ARCHITECTURES` PRIMA di `project()` (controllo: `otool -l $EXE | grep minos`, `lipo -archs`). `-mcpu` solo via `-Xarch_arm64`.
- `.mm`: `<Cocoa/Cocoa.h>` prima di JUCE; `DONT_SET_USING_JUCE_NAMESPACE=1` → sempre `juce::`. Stringhe UTF-8: `"\xe2\x8c\x98" "C"`.
- `static const` pair con `std::initializer_list` = lista pendente: usare `std::vector`.
- PopupMenu async: mai catturare `this`/plugin raw (SafePointer + rilettura). Stringhe tra thread solo sotto lock. CallOutBox con parent = top-level.
- `RackComponent` ha `setBufferedToImage`: un repaint del genitore non basta → `MainComponent::repaintEverything()`.
- Lavoro per-frame nel paint costa caro (es. `paramtwins` → `PairCache`); ridisegnare solo su cambiamento.
- Figli dello stesso eseguibile (`--scan-plugin`, `--probe-latency`, `--coreaudio-probe`, `--srk-sandbox:`) devono marcarsi background/accessory prima di NSApplication (`main()` è nostro, Main.cpp) o compaiono nel Dock.
- coreaudiod bloccato (driver difettosi, Pro Tools Audio Bridge dopo kill): non è un bug nostro; l'app regge grazie a `CoreAudioProbe` (test: `STAGERACK_TEST_COREAUDIO_HANG=<s>`).
- La sandbox di Bash non legge `~/Library/Caches/...`: serve `dangerouslyDisableSandbox`.
- zsh non fa word-splitting di `$var`. `osascript quit` non raggiunge un'istanza lanciata da shell.
- Computer-use in background: niente ⌘/⌥, i popup JUCE e i click raw sugli slot non arrivano; i menu e gli AXButton sì. Per aprire una sessione: impostare `lastSession` nelle settings con l'app chiusa. Provare la GUI scrive `lastSeenVersion` nelle settings vere: ripristinarle.
- Icona: `scripts/make-icon.sh` (NSImage via `render-svg.swift`); NON `ICON_BIG` di JUCE né `qlmanage`. Build locali = `AppIcon-Dev.icns`, `package.sh` rimette quella normale.

## Non verificato / aperto
- Dal vivo con audio vero: vista meter in movimento, EverClean nell'app GUI, automixer con microfoni veri, Dante (scollega/ricollega, cambio scheda dalla sessione).
- MIDImix vero (numeri CC da memoria), console luci via MIDI/OSC, OSC dalla rete.
- Molti gesti GUI (⌥/⇧-click, drag nella TopBar, popup OUT con ⌘-click, selettore colore custom, tap da finestra plugin) verificati solo nei test/render.
- Timeout di uscita con un blocco vero di De-Feedback (non riproducibile).
- M3 base dell'amico: servono i suoi numeri.
