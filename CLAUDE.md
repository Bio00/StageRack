# StageRack — contesto per Claude (leggi prima di tutto)

Host di plugin per audio LIVE stile Waves SuperRack Performer (+ idee da LiveProfessor). C++20 / JUCE 9.0.5. **App universale arm64+x86_64, macOS 11+** (tutti i Mac).
Questo file riassume tutto il lavoro fatto: NON ri-esplorare il codice o ripetere esperimenti già documentati qui.

## Utente
- Fonico live, scrive in italiano: rispondi in italiano, conciso. Mac **M4 Pro: 8 P-core (CPU 4-11) + 4 E-core (CPU 0-3)**.
- Scheda attuale: **Dante Virtual Soundcard** (~9-10 ms RT già da sola; apertura device lenta ~20 s dentro coreaudiod: non è un bug nostro). A volte usa "Altoparlanti MacBook Pro".
- Plugin chiave: **Alpha Labs De-Feedback** (VST3 only) ed EverClean; 578 plugin scansionati (AU, VST3, CLAP, LV2).
- Priorità assoluta: **latenza minima, zero latenza aggiunta** dal processing (ha rifiutato la modalità ASYNC per questo). Buffer realistico 32 campioni (anche 16).
- Vuole: CPU leggera, multicore pieno, misure concrete, verifica a vista. Apprezza spiegazioni del "perché".

## Build / run / test
```bash
cd /Users/bio/Documents/CLAUDE/StageRack
cmake --build build                      # Ninja, Release; riconfigura: cmake -S . -B build -G Ninja
open build/StageRack_artefacts/Release/StageRack.app
EXE=build/StageRack_artefacts/Release/StageRack.app/Contents/MacOS/StageRack
scripts/package.sh                       # dist/StageRack.app firmata ad-hoc + zip per altri Mac
scripts/release.sh [note.md]             # package + test + tag v<versione> + release GitHub (serve gh login)
$EXE --check-update [--download] [--fake-version 0.0.9]   # updater: cosa vede su GitHub / scarica e verifica
arch -x86_64 $EXE --selftest             # prova la slice Intel via Rosetta
$EXE --selftest                          # catena, latenza zero, cicli, compensazione, mono out, gruppi di latenza
$EXE --bench-device VST3 "/Library/Audio/Plug-Ins/VST3/Defeedback.vst3" <racks> <buffer> Altoparlanti --multicore-only [--per-rack N] [--noalign] [--nowake|--wake-always] [--timeline] [--trace]
$EXE --spread-test VST3 <plugin> <buffer> [wavDir] [--input file sec]   # null test spread on/off
$EXE --handover-test VST3 <plugin> <buffer> [--input file sec]           # passaggio live senza click
$EXE --bench VST3 <plugin> <racks> <buffer> [asyncBlock safetyMs perRack] [--async-only] [--params]   # offline, meno realistico
```
- `--bench-device` = CoreAudio reale (workgroup vero), uscita muta, rumore in ingresso: **è l'unico benchmark affidabile** (quello offline era pessimista: niente workgroup/CLPC).
- Dati app: `~/Library/Application Support/StageRack/` (`StageRack.settings` XML con `lastSession`, `verticalLayout`, `audioDevice`, `hopAlign`, `wakeAssist`...; `plugins.xml`). Sessione di test: `~/Documents/StageRack-Test.stagerack`.
- Bundle id `com.bio.stagerack`. Repo git pubblico https://github.com/Bio00/StageRack (release pubblicate: v0.1.0-beta, v0.2.0-beta con updater, v0.3.0-beta con autosalvataggio; nuove versioni con scripts/release.sh; autore = noreply GitHub, mai la Gmail); JUCE e CLAP sono **submodule** in `libs/` (JUCE è AGPLv3/commerciale: va ricordato quando si distribuisce).
- `$EXE --edit-selftest` = test delle modifiche con ⌥ sui RackComponent veri (finestra fuori schermo, `ModifierKeys::currentModifiers` + MouseEvent sintetici, AUDelay); esce con `_Exit` (non salva plugins.xml).

## Architettura (Source/)
- `Engine/WorkerPool` — fork/join realtime: worker TC-policy (period=blocco, computation 60%), joinano l'**AudioWorkgroup** CoreAudio; batch pubblicato con un atomic 64-bit; spin breve poi futex. Default worker = P-core − 1.
- `Engine/AudioEngine` — callback: rack processati a **stadi** (grafo: rack che leggono da altri rack = stadio successivo, stesso callback, latenza 0); modalità parallela adattiva (fan-out solo se il picco di carico lo giustifica); master; MIDI PC→scena, CC next/prev; `alignAllRacks/alignNewPlugin/realignRack` (HopAligner), `choosePhase` (penalizza fasi usate da rack in serie), `armWakeAssist`, `setRackInputSource` (+cycle check), `updateLatencyCompensation` (gruppi A-H, chiamata 2×/s dal timer UI).
- `Engine/Rack` — 8 slot in serie, I/O (mono/stereo in/out, `inputRackId` per catena, OUT none=-1), gain smussato, meter, delay line compensazione (max 32768), per-slot `hopCalls/hopPhase` atomici, `shownParams/paramsChosen` (UI). Contiene ancora codice ASYNC (factor ×2/4/8) **disattivato dall'UI**: le sessioni lo caricano sempre SYNC.
- `Engine/HopAligner` — misura costo per chiamata, trova l'hop periodico del plugin, lo sposta con N blocchi di silenzio (una volta, prima di andare live) nella fase meno occupata. Latenza 0.
- `Engine/CoreWakeAssist` — 1 thread spinner (QoS default −15) armato ~2 ms prima di un hop, solo se prima c'è una pausa >0.8 ms.
- `Engine/Session` — scene (snapshot stati plugin+mix), save/load `.stagerack` (XML), `rackToTree/pluginToTree/createRackFromTree/loadPluginFromTree`, copia/move plugin, duplica rack, I/O consecutivi.
- `Engine/Autosave` — marker `running.lock` (presente all'avvio = crash) + `Autosave.stagerack` nella cartella dati: timer 1 s, serializza ogni 15 s solo se `Session::toTree()` è cambiato rispetto a salvato/caricato/ultimo autosave (`getDocumentGeneration`), mai con `getPendingLoads()>0` o durante il respread. `MainComponent::offerRecovery` chiede Restore/Open last saved PRIMA di caricare plugin; `loadFromTree(root, file, markAsChanged)`. `cleanExit()` nel distruttore di MainComponent. Test in `--selftest` (cartella temporanea via `setDirectoryForTests`).
- `Plugins/PluginManager` — cache latenze (`latency.xml`, chiave `createIdentifierString`, registrata dagli slot al caricamento + probe `--probe-latency` in processi figli), formati JUCE + **CLAP host scritto da noi** (`CLAPPluginFormat.mm`, JUCE non ospita CLAP); scansione **out-of-process** (`--scan-plugin`, figlio esce con `_Exit`, l'XML vale anche se crasha all'uscita). VST2 solo con `-DSTAGERACK_VST2_SDK_PATH`.
- `Update/Updater` — aggiornamenti dalle release GitHub di Bio00/StageRack (pre-release incluse): `fetchLatest` (API releases, asset .zip con "macOS"), `compareVersions` (semver), `download`, `unpackAndVerify` (ditto, bundle id, versione = CFBundleShortVersionString, codesign), `launchInstaller` (script in `~/Library/Caches/StageRack/Update`: aspetta il PID, swap con backup, osascript admin se serve, `open`). `Controller` in MainComponent: controllo 3 s dopo l'avvio (setting `checkUpdates`, `skippedUpdate`), menu app "Check for Updates..." e AUDIO/DSP "Check now"; l'installer parte in `shutdown()` via `setPendingInstall` (annullato se l'utente annulla l'uscita). Versione = `STAGERACK_VERSION` da CMake (`project VERSION` + `STAGERACK_VERSION_LABEL`), deve coincidere col tag.
- `UI/` — `MainComponent` (layout H/V, scorciatoie, `setLightTheme`), `RackComponent.cpp` (ParamRow, SlotComponent con viste 0 parametri/1 mini/2 meter, rack), `PluginBrowser` (sidebar categoria/produttore stile Pro Tools, pallino latenza; `categoryOf` deduce la categoria dal nome perché gli AU dicono solo "Effect"), `PluginMeters.mm` (meter dei plugin: AU letti con `AudioUnitGetParameter` perché il valore JUCE è in cache e non si aggiorna; VST3 via IEditController dall'IComponent; filtro "junk" per i Waves), `EditController` (selezione, clipboard, comandi), `TopBar`, `SceneListComponent`, `PluginWindow`, `PluginManagerWindow` (+Settings), `PluginThumbnail.mm` (mini-view editor negli slot: aggiunta da un'altra sessione, verificarne lo stato), `Theme.h` (colori = variabili `inline`, `theme::setLight()` + `LookAndFeel::applyColours()`; i componenti che fanno `setColour` con colori del tema lo rifanno in `lookAndFeelChanged()`).

## Funzioni presenti
Niente master fader (rimosso su richiesta; sessioni vecchie caricano master = 0 dB). Tema chiaro (AUDIO/DSP → Light theme, setting `lightTheme`). Vista meter negli slot. Browser plugin stile Pro Tools con latenza.
Rack H/V con dimensione per-rack (bordo trascinabile, ⌘=/⌘-, layout compatto); scene + MIDI; scan SCAN ALL; browser plugin con ricerca; selezione rack (click/⌘/⇧/⌘A/Esc) e slot; **⌥ = modifica tutti i rack selezionati** (gain uguale, ⌥⇧ relativo; M/S/B; IN/OUT uguali, ⌥⇧/⌥⌘ consecutivi dal canale scelto a partire dal rack cliccato; LED/vista/parametri dello stesso plugin) via `EditController::linkedRacks/linkedSlots`, solo se il rack cliccato è selezionato; ⌥-click sul gain non azzera più (setDoubleClickReturnValue con modificatori vuoti, velocity mode senza modificatori); ⌘C/⌘V/⌘D/⌫; drag plugin tra qualsiasi rack (⌥=copia); drag rack (⌥=duplica); I/O consecutivi ⌘⌥I/⌘⇧I/⌘⌥O/⌘⇧O; OUT mono (L+R)/2; menu ••• e tasto destro ovunque sul rack; rack concatenati ("From rack" nel menu IN); gruppi di latenza A-H con badge "LG A +x ms"; parametri plugin sugli slot (barre trascinabili, ⇧ fine, rotella, doppio click default, on/off come toggle; default "intelligenti" Mix/Strength/Threshold...; scelta via "Parameters on rack"); riapertura ultima sessione; meter per-core; info per rack (core, %, picco, latenza).

## Scoperte tecniche (misurate — non ripetere)
**De-Feedback**: hop di rete neurale ogni **192 campioni** (indipendente dal buffer); quasi tutto il calcolo nel callback dell'hop (0.17 ms a clock alto, ~0.45 ms a clock basso), negli altri ~0.02 ms. Usa cblas_sgemm (Accelerate/AMX). Crea 2 thread/istanza: un **helper** (cond_wait, divide il lavoro dell'hop col thread audio che lo aspetta su mutex) e un thread licenza (`usleep(1s)`, irrilevante). Latenza 0. Il "Safe Mode" citato da Alpha Labs è del driver Focusrite/Windows, non del plugin.
1. **Hop allineati = picchi**: istanze avviate insieme fanno l'hop nello stesso callback → con 8 istanze ~24% callback persi anche a 32. Soluzione: **HopAligner** (spread fasi). `prepareToPlay` azzera la fase → riallineare dopo ogni prepare (già fatto).
2. **Paradosso "8 istanze meglio di 1"**: con poche istanze i P-core vanno in idle profondo; risvegliare il core per l'helper costa ~0.25 ms → 58% dell'hop era attesa (misurato: wall 0.444 ms vs CPU thread 0.184 ms). Soluzione: **CoreWakeAssist** (thread a priorità più BASSA dell'helper: tiene acceso un core, l'helper lo prelaziona). Costo ~+50% di un core con 1-4 istanze, ~0 con molte.
3. **Workgroup CoreAudio è decisivo**: con deadline reali il CLPC alza il clock (hop ~40% più veloce che offline).
4. Istanze in serie nello stesso rack: `choosePhase` le mette in fasi diverse (costo 1000 per rack in serie vs 1 per indipendenti) → 1 rack×2 = 2 rack×1.
5. **Scartati (nessun effetto o peggio)**: spin dei worker realtime, promozione TC/QoS override (`pthread_override_qos_class_start_np`) dei thread del plugin, ballast tempo o calcolo sul thread IO, AMX warm-up, carico in background, `VECLIB_MAXIMUM_THREADS=1`, offload dei rack ai worker, ASYNC (funziona ma aggiunge latenza: rifiutato dall'utente).
6. **Spread e suono** (`--spread-test`, null test): De-Feedback deterministico; con fasi diverse livello ≤0.26 dB, lag 0, ma null solo −22…−30 dB perché l'uscita dipende dalla griglia dei 192 campioni rispetto all'audio (griglia comunque casuale anche senza spread). Il problema udibile era il toggle che riavviava il motore (reset plugin + silenzio). Ora: OFF non tocca l'audio; ON / Re-spread = **handover live** (`Session::LiveRespreader` + `Rack::beginHandover`: gemello con stesso stato, allineato offline, 2 s in parallelo, crossfade 80 ms, `PluginWindow::replace`). `--handover-test`: bit-identico prima del fade, nessun click. Distruggere istanze VST3 fuori dal message loop crasha (Timer): nei test CLI tenerle vive e uscire con `_Exit`.
7. Risultati attuali (scheda reale, 10 s): 1-12 De-Feedback a 16 e 32 campioni → **0 callback oltre budget**; picchi ~0.2 ms @16, ~0.25-0.5 ms @32. Element (kushview) ha lo stesso modello (worker nel workgroup) e nulla di specifico per questi plugin.

## Trappole già incontrate
- **Crash di De-Feedback 1.1.4** (heap corruption "memory corruption of free block" in `VST3ComponentHolder::initialise` quando si crea un'istanza con altre già caricate, vicino a `SK_ApiContextSetFieldString` = libreria licenze): visto anche in `--spread-test` (codice host banale, single-thread) → bug del plugin, raro (0 su 112 creazioni, anche con Guard Malloc). Mitigato con l'autosalvataggio; da segnalare ad Alpha Labs.
- `juce::File::replaceWithText` scrive **CRLF** di default: per script bash passare `lineEndings = "\n"` (l'installer falliva in silenzio, il log si chiamava `install.log\r`).
- La sandbox di Bash di Claude non legge `~/Library/Caches/...` (ENOENT anche se `ls` lo mostra): usare dangerouslyDisableSandbox per leggere il log dell'updater.
- "Pro Tools Audio Bridge" può restare minuti dentro coreaudiod all'apertura dopo un kill brusco di StageRack: non è un bug nostro.
- Il callback CoreAudio di JUCE riceve **solo i canali attivi, compattati**; i rack usano i numeri di canale del device → `AudioEngine::setChannelMaps` (in `audioDeviceAboutToStart`) rimappa device→buffer. Bug trovato nel debug pre-beta (con canali disattivati nel setup, IN/OUT leggevano/scrivevano il canale sbagliato); test in `--selftest`.
- Build ASan/UBSan (Debug, arm64) per il debug: tutti i test + handover De-Feedback + app GUI puliti alla beta 0.1.0.
- `static const std::pair<..., std::initializer_list<...>> x[] = {...}`: la lista copiata nella pair resta pendente → crash (successo in `categoryOf`). Usare `std::vector`.
- Click "raw" in background (computer-use) NON arrivano agli slot JUCE: per provare viste/impostazioni degli slot, modificare la sessione (`view="2"` sull'elemento `<PLUGIN slot=...>`) e riaprirla. Per il browser c'è `--render-browser`.
- L'utente può usare l'app aperta mentre lavori: prima di chiuderla o di toccare i file in Application Support controlla che non sia in uso.
- `CMAKE_OSX_DEPLOYMENT_TARGET/ARCHITECTURES` vanno messi PRIMA di `project()`: prima era dopo e il binario usciva con **minos 26.0** (solo macOS 26!) e solo arm64. Controllo: `otool -l $EXE | grep minos`, `lipo -archs $EXE`. `-mcpu=apple-m1` solo via `-Xarch_arm64`. Spin: `__builtin_arm_yield` / `_mm_pause` per architettura. Sotto Rosetta lo scanner figlio va lanciato con `/usr/bin/arch -x86_64`.
- `JuceHeader` + Cocoa: in `.mm` importa `<Cocoa/Cocoa.h>` PRIMA degli header JUCE; definito `DONT_SET_USING_JUCE_NAMESPACE=1` → usa sempre `juce::`.
- JUCE 9: `addDefaultFormatsToManager(fm)`; moduli headless separati (`juce_audio_processors_headless`); VST2 hosting richiede header SDK.
- CallOutBox: lanciarla con parent = top-level component (da desktop si chiude se l'app perde focus).
- Callback di PopupMenu asincroni: mai catturare `this`/plugin raw → `SafePointer` e rilettura al click (causa di use-after-free). Stringhe condivise tra thread solo sotto lock (un crash "memory corruption of free block" veniva da qui).
- `outputChannel = -1` va saltato esplicitamente (bug corretto: R finiva su out 1).
- Stringhe UTF-8 in C++: `"\xe2\x8c\x98" "C"` (una lettera hex dopo l'escape rompe la compilazione).
- zsh non fa word-splitting di `$var`: nei loop usa `set -- $(echo $cfg)` o `bash -c`.
- Plugin con `prepareToPlay` resettano lo stato interno; `setStateInformation` di De-Feedback contiene solo Strength/Mute/Bypass.
- Computer-use in background: niente ⌘/⌥ né combo da tastiera, niente modificatori sul drag; i pulsanti AXButton (••• , VERTICAL) e i menu JUCE funzionano. Per aprire una sessione di test: impostare `lastSession` nelle settings con l'app chiusa (l'app riscrive le settings all'uscita).

## Non ancora verificato / aperto
- Vista meter con **audio vero**: valori in movimento non visti (a riposo erano 0, tranne l'ago di Waves SSLComp a 800 = 0.8, semantica ignota). Tema chiaro verificato a vista su finestra principale e impostazioni (cambio live e all'avvio).
- Probe latenze in background (thread + figli) non lanciato su tutti i 578 plugin; il figlio singolo è verificato.
- Prove con **audio reale in ingresso** (l'uscita interna non ha input); EverClean nel bench risultava quasi a costo zero (forse non attivo/licenza).
- Scorciatoie ⌘, alt-drag e multi-selezione ⌘/⇧-click non provate dal vivo (stesso codice dei menu, che è verificato).
- Mini-view/thumbnail (`PluginThumbnail`) e modifiche recenti di TopBar/AudioEngine fatte da un'altra sessione: rileggere prima di toccarle.
- Codice ASYNC dormiente in `Rack` (rimovibile se si vuole snellire).

## Modo di lavorare che funziona
Build dopo ogni modifica (`cmake --build build 2>&1 | grep -E "error" -A3`), `--selftest`, poi `--bench-device` per le prestazioni, poi verifica a vista (app_screenshot su `com.bio.stagerack`). README.md è la documentazione per l'utente: aggiornala con ogni funzione/misura nuova.
