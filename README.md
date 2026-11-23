<img src="Resources/Icon/StageRack-1024.png" width="128" alt="Icona di StageRack">

# StageRack

Host di plugin per l'audio dal vivo, stile Waves SuperRack Performer (rack, snapshot, interfaccia scura) con idee da LiveProfessor e Ableton. App universale (Apple Silicon e Intel), **macOS 11+**. In beta: provala prima di portarla su un palco vero.

- **[Manuale](docs/MANUALE.md)**: tutte le funzioni, scorciatoie e test.
- **[Misure](docs/MISURE.md)**: multicore, De-Feedback/EverClean, stabilità, automixer a confronto con Dugan.
- **[Novità](CHANGELOG.md)**.

## Formati
AU (v2/v3), VST3, CLAP (host scritto da zero: JUCE non ospita CLAP), LV2. VST2 opzionale (serve l'SDK, vedi sotto).

## In breve
- **Latenza zero aggiunta**: tutti i rack, le catene, i bus e i send sono elaborati nello stesso callback della scheda.
- **Multicore**: rack in parallelo su thread realtime nell'Audio Workgroup di CoreAudio (P-core), adattivo: un core solo quando il carico è basso.
- **Plugin pesanti a blocchi** (Alpha Labs De-Feedback, Everworks EverClean...): StageRack misura ogni plugin, distribuisce gli "hop" nei callback meno occupati (spread, anche dal vivo senza click) e tiene sveglio un core solo per chi ne ha bisogno. 16 De-Feedback a 32 campioni: 0 callback persi.
- **Rack** H/V da 8 slot, trim IN, gain, M/S/B, meter, I/O mono/stereo, uscite multiple, rack concatenati, gruppi di latenza A-H.
- **Bus, send e cartelle** stile Ableton (attivabili per sessione), viste per rack stile Pro Tools.
- **Automixer** a gain sharing (stile Dugan), gruppi A-D, finestra con i meter.
- **Snapshot** con richiamo MIDI (Program Change, CC), **tap tempo** (fino a 9 tempi, MIDI clock, OSC, rilevamento dall'audio, uscita MIDI/OSC), **mapping MIDI** di qualsiasi controllo (profili MIDImix, nanoKONTROL2).
- **Editing**: selezione multipla, ⌥ = modifica tutti i selezionati, gruppi di modifica, drag & drop di plugin e rack, undo/redo, parametri e meter dei plugin sugli slot.
- **Sicurezza dal vivo**: autosalvataggio e recupero dopo un crash, contatore e log dei dropout, avvisi di sistema (Low Power, temperatura, memoria), plugin isolati in un processo separato (sperimentale), avvio anche con coreaudiod bloccato.
- **Aggiornamenti** dalle release GitHub con verifica della firma e changelog in app.

## Aggiornamenti
All'avvio StageRack controlla GitHub e chiede prima di scaricare (**Download and install** / **Later** / **Skip this version**); manuale da menu StageRack → **Check for Updates...** o SETTINGS. Il pacchetto viene verificato (bundle id, versione, firma) e installato nella stessa cartella, con ritorno alla versione vecchia se qualcosa va storto. **Non aggiornare durante uno spettacolo**: l'audio si ferma qualche secondo.

Pubblicare una versione: alza `project(StageRack VERSION x.y.z)` in `CMakeLists.txt`, aggiungi in cima a `CHANGELOG.md` la sezione `## x.y.z-beta — data`, commit, poi `scripts/release.sh` (compila, testa, impacchetta, tag e release GitHub; il tag deve coincidere con la versione).

## Build
Servono i Command Line Tools e `brew install cmake ninja`.
```bash
git clone --recursive https://github.com/Bio00/StageRack.git
```
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```
```bash
cmake --build build
```
L'app è in `build/StageRack_artefacts/Release/StageRack.app` (icona con fascia DEV per non confonderla con quella installata). L'icona si rigenera dagli SVG in `Resources/Icon` con `scripts/make-icon.sh`.

**VST2**: copia `aeffect.h` e `aeffectx.h` (vecchi VST3 SDK ≤ 3.6.x) in una cartella e passala con `-DSTAGERACK_VST2_SDK_PATH=/percorso`.

## Usarla su altri Mac
```bash
scripts/package.sh
```
Crea `dist/StageRack.app` firmata ad-hoc e lo zip da copiare. Sull'altro Mac, la prima volta: tasto destro → **Apri** (non c'è un Developer ID Apple). I thread DSP si adattano al Mac (P-core − 1; sugli Intel core fisici − 1). Su Apple Silicon i plugin solo-Intel richiedono "Apri con Rosetta".

## Licenza
StageRack usa [JUCE](https://juce.com) (AGPLv3 o commerciale), quindi è rilasciato sotto **GNU AGPLv3** ([`LICENSE`](LICENSE)). CLAP SDK: MIT.
