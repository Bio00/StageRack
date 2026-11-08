# StageRack

> **Beta 0.1.0** — prima versione pubblica di prova. Usala in prova prima di portarla su un palco vero.

Host di plugin per l'audio dal vivo, per macOS 11 (Big Sur) o successivo, su **tutti i Mac**: app universale, nativa sia su Apple Silicon (M1…M4) sia su Intel. L'impostazione è quella di Waves SuperRack Performer (rack, scene, interfaccia scura), con alcune idee prese da LiveProfessor.

## Formati supportati
| Formato | Stato |
|---|---|
| AU (AudioUnit v2/v3) | ✅ |
| VST3 | ✅ |
| CLAP | ✅ host scritto da zero (`Source/Plugins/CLAPPluginFormat.mm`): JUCE non sa ospitare plugin CLAP |
| LV2 | ✅ |
| VST2 | ⚙️ opzionale: Steinberg non distribuisce più l'SDK VST2, quindi va aggiunto a mano (vedi sotto) |

## Multicore
- Ogni **rack** è una catena di 8 slot in serie. I rack sono indipendenti tra loro, quindi vengono **elaborati in parallelo**.
- `WorkerPool`: thread realtime (time-constraint) agganciati all'**Audio Workgroup di CoreAudio**, così macOS li tiene sui **P-core** dell'M4 Pro insieme al thread dell'interfaccia audio. Di default usa un thread per P-core.
- Il percorso audio non usa lock né allocazioni: il lavoro viene distribuito tramite un unico atomico a 64 bit.
- **Adattivo**: se il carico è basso resta su un solo core (svegliare i thread costa più di quanto si guadagna) e passa al multicore quando il *picco* recente lo giustifica. Da fermo i thread dormono e il consumo di CPU è circa 0%.
- Nella barra in alto c'è un meter per ogni thread; ogni rack mostra il core su cui gira, la sua CPU e la latenza.

Benchmark su M4 Pro, 128 campioni @ 48 kHz:
```
soothe2 ×16 (8 rack):        1 thread 0.38 ms (worst 1.61)  →  8 thread 0.074 ms (worst 0.44)   x5.1
Supermassive ×24 (12 rack):  1 thread 0.25 ms               →  8 thread 0.059 ms                 x4.2
```
Per ripeterlo:
```bash
build/StageRack_artefacts/Release/StageRack.app/Contents/MacOS/StageRack --bench VST3 "/Library/Audio/Plug-Ins/VST3/soothe2.vst3" 8 128
```

## Plugin pesanti a latenza zero (De-Feedback, ...)
### Come lavora De-Feedback (misurato)
- Esegue la sua rete neurale una volta ogni **192 campioni** (un "hop"). In quel callback spende 0.2-0.7 ms; negli altri quasi niente (~0.02 ms).
- Ogni istanza crea 2 thread propri e usa il BLAS di Accelerate. Il thread audio esegue gran parte del calcolo dell'hop.
- Senza accorgimenti, tutte le istanze fanno l'hop **nello stesso callback**: 8 istanze = 8 hop insieme = callback persi, anche con tutti i core.

### Cosa fa StageRack (senza aggiungere latenza)
1. **Spread degli hop** (AUDIO / DSP, attivo di default). Quando un plugin viene inserito o la scheda riparte, StageRack ne misura il costo chiamata per chiamata, riconosce l'hop e gli fa processare qualche blocco di silenzio una volta sola, così il suo hop cade nella fase di callback meno occupata. Il tempo di uscita del plugin non cambia: la latenza resta 0.
2. **Core wake assist** (AUDIO / DSP, Auto di default). A ogni hop De-Feedback sveglia un proprio helper interno e lo aspetta. Con poche istanze i P-core sono in idle profondo e svegliarne uno richiede ~0.25 ms: **il 58% dell'hop era attesa** (misurato confrontando tempo CPU e tempo reale del thread audio). Per questo 8 istanze andavano meglio di 1: con più istanze i core restano accesi. StageRack conosce in anticipo il callback di ogni hop (punto 1) e circa 2 ms prima avvia un thread a priorità più bassa di quella dei plugin, che tiene acceso un P-core; l'helper lo prelaziona subito. Si attiva solo se prima dell'hop c'è una pausa abbastanza lunga da far addormentare i core, quindi con molte istanze resta spento.
3. **Rack in parallelo** su thread realtime nel workgroup CoreAudio.

Cosa non ha funzionato (misurato e scartato): spin dei worker realtime, promozione a realtime o QoS dei thread del plugin, ballast di calcolo sul thread audio, tenere caldo il coprocessore AMX, carico in background, BLAS a thread singolo.

### Misure sulla scheda reale (CoreAudio, workgroup attivo, 48 kHz)
Callback oltre il budget in 10 s:

| De-Feedback | 16 campioni (0.33 ms) | 32 campioni (0.67 ms) | CPU processo |
|---|---|---|---|
| 1 | **0** (picco 0.22 ms) | **0** (picco 0.24 ms) | ~70-78% |
| 2 | **0** | **0** | ~79% |
| 4 | **0** | **0** (picco 0.32 ms) | ~115-123% |
| 8 | **0** | **0** | ~168-180% |
| 12 | **0** | **0** | ~213% |

Prima del core wake assist, 1-4 istanze a 16 campioni perdevano da 1000 a 3600 callback ogni 10 s; senza spread degli hop, 8 istanze perdevano ~24% dei callback anche a 32.

Per ripetere le misure sulla scheda interna del Mac (uscita in silenzio, rumore in ingresso):
```bash
build/StageRack_artefacts/Release/StageRack.app/Contents/MacOS/StageRack --bench-device VST3 "/Library/Audio/Plug-Ins/VST3/Defeedback.vst3" 1 16 Altoparlanti --multicore-only
```
Opzioni: `--noalign` (senza spread), `--nowake` / `--wake-always` (assist spento / sempre acceso), `--timeline` (costo dell'hop nel tempo).

### Più istanze nello stesso rack
### Lo spread cambia il suono? (misurato)

No, in nessun modo che dipenda dallo spread. Test `--spread-test` (stesso segnale in istanze identiche, con e senza spread) sulla registrazione `Castello_21-5_Panel-4.wav`, buffer 32:
- De-Feedback è deterministico: due istanze senza spread danno uscite identiche bit per bit.
- Con lo spread: **livello uguale** (scarto ≤0.26 dB, cioè nella variabilità normale), **latenza uguale** (correlazione ingresso→uscita a 0 campioni per tutte le fasi), **nessun jitter**.
- L'uscita non è identica campione per campione (null −22…−30 dB): De-Feedback lavora a blocchi di 192 campioni e il risultato dipende da dove cade la griglia dei blocchi rispetto all'audio. Ma questa griglia è **sempre casuale**, anche senza spread: dipende dall'istante in cui il plugin è stato caricato o la scheda è ripartita. Spostarla di 192 campioni dà lo stesso risultato (−48 dB). Lo spread sceglie solo un'altra griglia, ugualmente valida.

Quello che si sentiva davvero era **il passaggio**: prima, attivare/disattivare lo spread riavviava il motore audio, quindi ogni plugin veniva resettato (De-Feedback ricomincia ad adattarsi da zero) e c'era silenzio per ~0.25 s per plugin durante la misura. Ora:
- **OFF** non tocca l'audio (non c'è niente da "disfare": la posizione dell'hop non cambia il suono); i plugin caricati dopo non vengono distribuiti.
- **ON** e **Re-spread now** lavorano **dal vivo, senza interruzioni**: per ogni plugin con un hop si crea un gemello con lo stesso stato e gli stessi parametri, lo si sposta di fase offline, lo si fa girare in parallelo sull'ingresso vero per 2 s (si adatta), poi un crossfade di 80 ms passa l'uscita al gemello e il vecchio viene eliminato. Un plugin alla volta; i parametri toccati nel frattempo vengono copiati; una finestra editor aperta passa al gemello nella stessa posizione. Stato visibile accanto al pulsante.
- Verificato con `--handover-test` (buffer 16/32/64, parlato e segnale sintetico): uscita **bit-identica** fino all'inizio del crossfade, nessun click (pendenza massima uguale al riferimento), livello durante il crossfade entro ±0.6 dB. Bench reale dopo la modifica: 8 De-Feedback a 32 campioni, 0 callback oltre budget.

```bash
$EXE --spread-test VST3 /Library/Audio/Plug-Ins/VST3/Defeedback.vst3 32 [cartellaWav] [--input file.wav secondi]
$EXE --handover-test VST3 /Library/Audio/Plug-Ins/VST3/Defeedback.vst3 32 [--input file.wav secondi] [--verbose]
```

Gli hop di plugin che girano **in serie** (stesso rack, o rack concatenati) si sommerebbero sullo stesso core. Lo spread li mette quindi prima in fasi diverse fra loro, poi bilancia con il resto. Risultato a 16 campioni: 1 rack × 2 De-Feedback, picco 0.21 ms, come 2 rack separati (0.22 ms); 1 rack × 4, picco 0.22 ms.

### Nota su Dante Virtual Soundcard
Con DVS l'apertura del device può richiedere ~20 s (attesa dentro `coreaudiod`), e DVS dichiara già ~9-10 ms di round trip anche a 32 campioni. Con una scheda CoreAudio fisica la latenza totale scende davvero.

## Concatenamento e gruppi di latenza
- **Rack concatenati**: nel menu IN di un rack, sotto "From rack", scegli un altro rack come sorgente. Il rack riceve l'uscita dell'altro (dopo gain e mute). Con OUT: none il rack sorgente alimenta solo la catena. Il motore elabora i rack a **stadi** (prima i rack che leggono dalla scheda, poi quelli che leggono da altri rack, ognuno in parallelo), sempre **nello stesso callback, quindi senza latenza aggiunta**. Le catene circolari sono rifiutate.
- **Gruppi di latenza** (come SuperRack): menu del rack → Latency group A-H. Ogni rack del gruppo viene ritardato fino alla latenza più alta del gruppo, compresa quella ereditata dai rack a monte, così restano allineati in fase. Il badge "LG A +x ms" mostra il ritardo applicato. Le latenze vengono ricontrollate due volte al secondo, perché un plugin può cambiarle.

## Slot dei plugin
- **Tasto on/off** (icona ⏻ verde a sinistra del nome): accende o spegne il plugin. Da spento lo slot si attenua e mostra "OFF".
- **Tasto vista** (a destra del nome; l'icona mostra la vista successiva): passa a rotazione fra **parametri**, **vista mini** e **meter**. Anche da tasto destro → **View**.
- **Vista meter**: solo i meter che il plugin pubblica (riduzione di guadagno, livelli in/out, VU). Negli slot larghi una riga per meter (nome, barra, valore), in quelli alti barre verticali; la riduzione di guadagno è in ambra e cresce dall'altro lato. Se il plugin non pubblica meter compare "No meters" (es. FabFilter Pro-Q/Pro-L, Smooth Operator: i loro meter esistono solo nella loro interfaccia). Come vengono trovati: AU = parametri marcati meter o di sola lettura, letti direttamente dall'Audio Unit; VST3 = flag "read-only" quando il controller è raggiungibile, altrimenti parametri non automatizzabili con nome da meter. Gli stati interni della GUI che alcuni produttori marcano come meter (Waves: "Dummy", marker, segmenti LED "Input_3"...) vengono scartati. Esempi: C1 → Out VU L/R, VU Peak, Comp Energy; CLA-76 → VU, Reduction Meter; AUDynamicsProcessor → Comp Amount, Input/Output Amplitude.
- **Vista mini**: una piccola immagine dell'interfaccia del plugin. L'immagine viene catturata senza permessi di registrazione schermo: la prima volta creando l'editor in modo invisibile, poi ogni 2 s mentre la finestra del plugin è aperta e alla sua chiusura, così riflette le impostazioni attuali. Se un plugin disegna con Metal/OpenGL e l'immagine risulta vuota, compare una scheda con nome e produttore.
- **Dimensione dello slot**: trascina il bordo destro (orizzontale) o inferiore (verticale); doppio click sul bordo per tornare alla dimensione standard. Quando serve spazio cedono prima gli slot vuoti. In uno slot più grande i parametri si dispongono **a griglia su più colonne**; il piè di pagina indica quanti ne restano nascosti ("+4 par.").
- Dimensione e vista sono salvate per ogni plugin e lo seguono quando lo sposti o lo copi.

## Parametri dei plugin sui rack
- Ogni slot mostra alcuni parametri del plugin come barre: trascina per regolare (⇧ per la regolazione fine), rotella, doppio click per il valore di default; i parametri on/off sono interruttori.
- Di default StageRack sceglie fino a 12 controlli principali (Mix, Strength, Threshold, Output, Gain, Frequency, Decay...), al massimo 2 dello stesso tipo nel primo giro, e scarta sidechain, MIDI, bypass e i flag interni; ne vedi tanti quanti ne entrano nello slot. Tasto destro sullo slot → **Parameters on rack** per scegliere quali mostrare; la scelta viene salvata nella sessione.
- I rack sono più alti di default per far stare 3 parametri per slot; con ⌘= / ⌘- o trascinando il bordo ne mostri di più o di meno. Negli slot stretti il valore compare passando con il mouse.

## Layout ed editing
- **VERTICAL / HORIZONTAL** nella barra in alto: rack in righe (stile SuperRack) o strisce verticali stile mixer.
- **Colonna delle scene richiudibile**: la striscia con la freccia a sinistra dei rack (o ⌘L) la chiude e la riapre; lo stato viene ricordato.
- **Dimensione dei rack**: trascina il bordo inferiore (orizzontale) o destro (verticale), oppure ⌘= / ⌘- sui rack selezionati (o su tutti). Da piccoli i rack passano a un layout compatto.
- **Selezione**: click sul rack, ⌘-click per aggiungere, ⇧-click per un intervallo, ⌘A per tutti, Esc per deselezionare. Click su uno slot per selezionare il plugin.
- **Trascina** un plugin su un qualsiasi slot di un qualsiasi rack per spostarlo (se lo slot è occupato si scambiano); **con ⌥ (Alt)** ne fai una copia con lo stesso stato.
- **Trascina un rack** (dal nome o dallo sfondo) per riordinarlo; **con ⌥** lo duplichi in quella posizione.
- **⌘C / ⌘V / ⌘D / ⌫**: copia, incolla, duplica, elimina. Agiscono sul plugin selezionato o sui rack selezionati. L'eliminazione dei rack chiede conferma.
- **I/O consecutivi** sui rack selezionati, partendo dal canale del primo: ⌘⌥I ingressi mono, ⌘⇧I ingressi stereo, ⌘⌥O uscite mono, ⌘⇧O uscite stereo.
- **Modifiche su tutti i rack selezionati (⌥ / Alt)**: seleziona i rack (⌘-click, ⇧-click, ⌘A), poi agisci su un controllo di **uno di essi** tenendo premuto ⌥:

  | Controllo | ⌥ (tutti uguali) | ⌥⇧ (oppure ⌥⌘) |
  |---|---|---|
  | Gain | stesso valore su tutti | **relativo**: sposta tutti della stessa quantità, mantenendo le differenze |
  | M / S / B | stesso stato su tutti | — |
  | Menu IN / OUT | stesso ingresso/uscita su tutti (anche "From rack" e "none") | **consecutivi** dal canale scelto: il rack su cui clicchi prende quel canale, i successivi i seguenti (mono +1, stereo +2); quelli sopra continuano in coda |
  | Accensione plugin (LED) | stesso stato sullo **stesso plugin** in tutti i rack selezionati | — |
  | Icona vista dello slot | stessa vista sullo stesso plugin | — |
  | Parametro sul rack (trascina, rotella, doppio click, on/off) | stesso valore sullo stesso parametro dello stesso plugin | — |

  Senza ⌥ il controllo agisce solo sul suo rack, e ⌥ su un rack **non** selezionato agisce solo su quello: niente modifiche di massa per sbaglio. Il ⌥-click su LED/vista non cambia la selezione, così puoi ripeterlo. Latency group e colore dal menu ••• valgono già per tutti i selezionati. (Il ⌥-click sul gain non lo azzera più: per lo 0 dB c'è il doppio click.)
- **Uscita mono**: nel menu OUT scegli "OUT n mono", che manda (L+R)/2 su un solo canale.
- Tutto è disponibile anche dal menu **•••** o dal tasto destro sul rack.

## Browser dei plugin (stile Pro Tools)
- Click su uno slot vuoto (o **Replace...**): a sinistra la colonna **CATEGORY** (EQ, Dynamics, Reverb, Delay, Modulation, Harmonic, Pitch Shift, Noise Reduction, Sound Field, Metering, Channel Strip, Instrument, Other) e **MANUFACTURER**, con il numero di plugin. Click per filtrare, di nuovo per togliere il filtro; categoria e produttore si combinano, insieme a ricerca e formato. Il browser riapre con gli ultimi filtri.
- La categoria viene dalla sotto-categoria VST3 quando il produttore la compila, altrimenti (quasi tutti gli AU dicono solo "Effect") è dedotta dal nome.
- **Latenza a colpo d'occhio**: pallino **verde** = 0 latenza, **giallo** = sotto 5 ms, **grigio** = 5 ms o più, cerchio vuoto = non ancora misurata; a destra il valore in ms.
- Le latenze si imparano da sole ogni volta che un plugin viene caricato in un rack; il pulsante **Measure latency (N)** misura tutti gli altri in background, caricandoli in processi separati (a 48 kHz, 3 alla volta, timeout 30 s; un plugin che crasha viene segnato "?" senza toccare l'app). Esempi misurati: De-Feedback 0, L4 1.5 ms, AUPeakLimiter 2 ms, Pro-L 2 65 ms. Cache in `~/Library/Application Support/StageRack/latency.xml`. Nota: la latenza di alcuni plugin cambia con le impostazioni (es. modalità lineari): il valore è quello di default o dell'ultimo caricamento.

## Tema chiaro
- AUDIO / DSP → **Light theme**: palette chiara per palchi all'aperto/di giorno; si applica subito a tutte le finestre e viene ricordata.

## Funzioni live
- **Scene**: salvano lo stato di tutti i plugin, gain, mute, solo e bypass. Si richiamano con click, frecce ←/→, Spazio, tasti 1–9, **MIDI Program Change** e CC next/prev (configurabili, default CC80/81).
- **Scansione fuori processo** e in parallelo: un plugin che crasha durante la scansione finisce in blacklist e l'host resta in piedi.
- Spostamento dei plugin tra slot con drag & drop; bypass dal LED.
- Routing di ogni rack: ingresso stereo o mono, uscita su qualsiasi coppia di canali; meter in/out. Non c'è master fader: ogni rack ha il suo gain (le sessioni vecchie col master vengono caricate a 0 dB).
- All'avvio riapre automaticamente l'ultima sessione (`.stagerack`).
- Menu del rack: pulsante **•••** oppure tasto destro in qualsiasi punto del rack.
- Scorciatoie: ⌘S salva, ⌘⇧S memorizza la scena, ⌘O apri, ⌘N nuova sessione, ⌘R nuovo rack.

## Test
```bash
build/StageRack_artefacts/Release/StageRack.app/Contents/MacOS/StageRack --selftest
```
Diagnostica UI: `--meters <descrizione.xml>` elenca i meter trovati in un plugin (descrizione = un elemento `<PLUGIN>` di plugins.xml); `--render-browser <cartella>` disegna il browser in tema scuro e chiaro in PNG senza aprire la scheda audio.

```bash
build/StageRack_artefacts/Release/StageRack.app/Contents/MacOS/StageRack --edit-selftest
```
Prova le modifiche con ⌥ sui componenti veri dei rack (finestra fuori schermo, eventi mouse e tasti modificatori simulati, 4 rack con AUDelay di Apple): gain uguale/relativo, M/S/B, IN "From rack"/none, consecutivi da un rack in mezzo, LED e parametri del plugin su più rack, rack non selezionati intatti.

Verifica: guadagni lungo una catena, canali disattivati nel setup della scheda (il callback li riceve compattati), latenza zero anche con la sorgente più in basso nella lista, rifiuto dei cicli, ritardo di compensazione esatto al campione, uscita mono, gruppo di latenza con un plugin reale (Smooth Operator Pro, 1536 campioni) ed eredità della latenza a monte.

## Build
Servono i Command Line Tools e `brew install cmake ninja`.
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```
```bash
cmake --build build
```
L'app viene creata in `build/StageRack_artefacts/Release/StageRack.app`: è **universale** (arm64 + x86_64) e richiede **macOS 11+** (impostati in `CMakeLists.txt`, prima di `project()`; il minimo 11 viene da `std::atomic::wait` e dagli Audio Workgroup).

## Usarla su altri Mac
```bash
scripts/package.sh
```
Crea `dist/StageRack.app` firmata ad-hoc (bundle sigillato) e `dist/StageRack-<versione>-macOS11-universal.zip` da copiare sull'altro Mac. Lì, la prima volta: tasto destro → **Apri** (o Impostazioni di Sistema → Privacy e sicurezza → "Apri comunque"), perché l'app non è firmata con un Developer ID Apple. Con un certificato Developer ID: `scripts/package.sh "Developer ID Application: ..."` e poi notarizzazione. Hardened runtime volutamente spento (bloccherebbe il caricamento dei plugin di terze parti).

Cosa si adatta da solo al Mac:
- **Thread DSP**: P-core − 1 su Apple Silicon (M1/M2/M3 base: 4 P-core → 3 worker; Pro/Max di più); sui Mac Intel tutti i core fisici − 1 (gli hyper-thread non contano). Etichette "P-core" mostrate solo dove esistono.
- **Plugin**: l'app nativa carica solo plugin con la stessa architettura. Su Apple Silicon i plugin solo-Intel non si caricano: per usarli, Finder → StageRack → Ottieni informazioni → "Apri con Rosetta" (anche la scansione out-of-process resta allora in x86_64).
- Verificato: `--selftest` passa sia arm64 sia x86_64 (Rosetta); De-Feedback ×2 @64 sotto Rosetta: 0 callback oltre budget.

**VST2**: copia gli header `pluginterfaces/vst2.x/aeffect.h` e `aeffectx.h` (presenti nei vecchi VST3 SDK, fino alla 3.6.x) in una cartella e passala a CMake:
```bash
cmake -S . -B build -G Ninja -DSTAGERACK_VST2_SDK_PATH=/percorso/vst2sdk
```

## Licenza
StageRack usa [JUCE](https://juce.com) (submodule in `libs/JUCE`), distribuito con licenza **AGPLv3** oppure con licenza commerciale JUCE: per questo **StageRack è rilasciato sotto GNU AGPLv3** (testo in [`LICENSE`](LICENSE)). Chi lo modifica e lo ridistribuisce deve mantenere la stessa licenza. CLAP SDK (`libs/clap`): MIT.

## Clonare e compilare
```bash
git clone --recursive <url-del-repo>
```
(se hai già clonato senza `--recursive`: `git submodule update --init`)

## Struttura
```
Source/Engine   WorkerPool (multicore), Rack, AudioEngine (callback), Session (scene, salvataggio)
Source/Plugins  PluginManager (formati, scansione out-of-process), CLAPPluginFormat
Source/UI       TopBar, SceneList, RackComponent (slot), PluginBrowser, PluginMeters, finestre plugin/impostazioni
```
