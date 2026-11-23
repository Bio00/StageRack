# StageRack — misure e dettagli tecnici

Misure su M4 Pro (8 P-core + 4 E-core), 48 kHz, salvo dove indicato. Comandi di benchmark: vedi `--bench-device` in [MANUALE](MANUALE.md#test).

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

## Plugin pesanti a latenza zero (De-Feedback, EverClean, ...)
StageRack non ha impostazioni scritte per un plugin specifico: **misura ogni plugin** che inserisci (tempo di ogni chiamata, regolarità dei picchi, attesa di thread interni) e si adatta. I plugin a costo costante (EQ, compressori...) non vengono toccati. L'unica regola fissa è accendere *Active* su EverClean all'inserimento, perché spento non fa nulla.

### Come lavora De-Feedback (misurato)
- Esegue la sua rete neurale una volta ogni **192 campioni** (un "hop"). In quel callback spende 0.2-0.7 ms; negli altri quasi niente (~0.02 ms).
- Ogni istanza crea 2 thread propri e usa il BLAS di Accelerate. Il thread audio esegue gran parte del calcolo dell'hop.
- Senza accorgimenti, tutte le istanze fanno l'hop **nello stesso callback**: 8 istanze = 8 hop insieme = callback persi, anche con tutti i core.

### Cosa fa StageRack (senza aggiungere latenza)
1. **Spread degli hop** (SETTINGS, attivo di default). Quando un plugin viene inserito o la scheda riparte, StageRack ne misura il costo chiamata per chiamata, riconosce l'hop e gli fa processare qualche blocco di silenzio una volta sola, così il suo hop cade nella fase di callback meno occupata. Il tempo di uscita del plugin non cambia: la latenza resta 0.
   **Controllo continuo degli hop**: durante lo show StageRack registra in quali callback ogni plugin ha davvero l'hop e lo confronta con dove l'aveva messo (2 volte al secondo, costo trascurabile). Se un hop compare o si sposta (plugin acceso dopo, come EverClean con *Active*, rack o slot tolto dal bypass, stato richiamato da una scena) e finisce in un callback già occupato mentre una fase più libera esiste, quel plugin viene ri-distribuito **dal vivo** con lo stesso passaggio senza interruzioni del Re-spread (gemello + crossfade, sotto). Un hop sparito da 1 s (plugin spento) libera la sua fase.
2. **Core wake assist** (SETTINGS, Auto di default). A ogni hop De-Feedback sveglia un proprio helper interno e lo aspetta. Con poche istanze i P-core sono in idle profondo e svegliarne uno richiede ~0.25 ms: **il 58% dell'hop era attesa** (misurato confrontando tempo CPU e tempo reale del thread audio). Per questo 8 istanze andavano meglio di 1: con più istanze i core restano accesi. StageRack conosce in anticipo il callback di ogni hop (punto 1) e circa 2 ms prima avvia un thread a priorità più bassa di quella dei plugin, che tiene acceso un P-core; l'helper lo prelaziona subito. Si attiva solo se prima dell'hop c'è una pausa abbastanza lunga da far addormentare i core, quindi con molte istanze resta spento. E solo per i plugin che durante l'hop **aspettano** davvero un loro thread: StageRack misura per ogni plugin tempo reale − tempo CPU della chiamata dell'hop (De-Feedback 0.02-0.13 ms → assist usato; EverClean 0.000 ms → assist saltato, niente CPU sprecata).
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

### EverClean (Everworks)
Gratuito e usato da molti tecnici al posto di De-Feedback. Misurato (`--plugin-probe`, `--bench-device`):
- **Si inserisce spento**: il parametro *Active* è Off e il plugin non fa nulla (costo ~0, nessun hop). Quando lo inserisci da StageRack viene **acceso automaticamente** (*Active* = On), prima della misura dell'hop. Le sessioni, le copie e le scene mantengono invece lo stato salvato.
- Acceso ha lo stesso schema di De-Feedback: hop ogni **192 campioni**, latenza 0, ~0.012 ms nelle chiamate normali. L'hop però è più pesante (0.21-0.27 ms offline, **0.31-0.36 ms dal vivo**) e lo calcola tutto il thread audio (nessun helper da aspettare: tempo reale = tempo CPU), quindi il core wake assist non serve.
- **Il contatore dei frame si ferma quando è spento**: se *Active* resta Off più di ~40 ms, alla riaccensione l'hop riparte da dove capita. Se si accendono più istanze insieme (scena, modifica ⌥ su più rack) gli hop finiscono tutti nello stesso callback. Lo corregge il controllo continuo degli hop (sopra).

Callback oltre il budget in 10 s (Altoparlanti MacBook Pro, 48 kHz, *Active* On):

| EverClean @32 campioni | con spread | senza spread |
|---|---|---|
| 1 | **0** (picco 0.41 ms) | 0 (0.48 ms) |
| 4 | **0** (0.46 ms) | |
| 8 | **0** (0.49 ms) | 27 (1.24 ms) |
| 12 | **0** (0.66 ms) | 1881 (1.35 ms) |

Accensione simultanea di 8 istanze spente (`--watch-test`): subito tutti gli 8 hop nello stesso callback (picco 0.75 ms, 5 callback persi nel primo secondo), poi 6 passaggi dal vivo, uno ogni ~2 s, e in ~13 s le fasi sono 2-2-1-1-1-1 (l'ideale per 8 su 6), picco 0.47 ms, **0 callback persi durante i passaggi**. Il passaggio di EverClean è pulito come quello di De-Feedback (`--handover-test`: bit-identico prima del crossfade, nessun click).

**A 16 campioni EverClean non ci sta**: anche una sola istanza ha un hop di 0.29 ms (mediana) / 0.35 ms (p90) contro un budget di 0.33 ms, e quel calcolo è in un solo thread dentro il plugin, quindi né lo spread né i core in più possono aiutare (1 istanza: ~2% di callback persi; 8 istanze: ~50%). Con EverClean usa **32 campioni** (o più). De-Feedback invece a 16 funziona perché divide l'hop col suo helper.

```bash
$EXE --plugin-probe VST3 /Library/Audio/Plug-Ins/VST3/EverClean.vst3 32 --set Active=On [--noise] [--insert] [--toggle-test Active]
$EXE --bench-device VST3 /Library/Audio/Plug-Ins/VST3/EverClean.vst3 8 32 Altoparlanti --multicore-only --set Active=On [--noalign]
$EXE --watch-test VST3 /Library/Audio/Plug-Ins/VST3/EverClean.vst3 8 32 Altoparlanti [--noalign] [--start-on] [--verbose] [--seconds 30]
$EXE --handover-test VST3 /Library/Audio/Plug-Ins/VST3/EverClean.vst3 32 --set Active=On
```
Nota trovata con questi test: i gemelli del re-spread live partono nel callback successivo, non 2 dopo come si assumeva: ogni re-spread dal vivo (anche con De-Feedback) finiva 2 fasi prima del previsto. Corretto, e se un gemello parte comunque in ritardo il thread audio gli dà i blocchi di silenzio mancanti prima che venga ascoltato.

### De-Feedback contro EverClean con audio reale
Stessa serie di misure, plugin alternati: ogni rack riceve un canale mono della registrazione di un panel (`Castello_21-5_Panel-4.wav`, da 600 s, ogni rack un punto diverso = microfoni diversi, picco −16.5 dBFS), Altoparlanti MacBook Pro, 48 kHz, 10 s di misura. CPU = secondi di CPU del processo (user+sys) in ~15 s di prova, creazione compresa.

| Istanze @32 | De-Feedback persi | picco | CPU | EverClean persi | picco | CPU |
|---|---|---|---|---|---|---|
| 1 | **0** | 0.26 ms | 9.7 s | **0** | 0.50 ms | 1.4 s |
| 4 | **0** | 0.31 | 14.5 | 1 | 0.69 | 9.0 |
| 8 | **0** | 0.48 | 22.0 | **0** | 0.46 | 16.9 |
| 12 | **0** | 0.50 | 28.9 | **0** | 0.50 | 22.4 |
| 16 | **0** | 0.52 | 34.0 | **0** | 0.51 | 29.2 |
| 8 senza spread | 1678 | 2.38 | | 0 | 0.65 | |
| 12 senza spread | 1681 | 3.42 | | 1912 | 1.01 | |

| Istanze @16 | De-Feedback persi (picco) | EverClean persi (picco) |
|---|---|---|
| 1 | **0** (0.22 ms) | 21-157 su 33000 (0.66 ms) |
| 2 | | 118 |
| 4 | **0** (0.20) | 2381 |
| 8 | **0** (0.22) | 10423 (~50%) |
| 12 | **0** (0.23) | |

- A 32 campioni sono equivalenti per i callback persi (0 fino a 16 istanze; il singolo callback perso con 4 EverClean è un evento isolato). EverClean usa **meno CPU**: 7 volte meno con 1 istanza (De-Feedback ha l'helper e il wake assist), 15-25% meno con 8-16.
- A 16 campioni solo De-Feedback: EverClean con 1-2 istanze perde 0.06-0.5% dei callback (click occasionali), da 4 in su è inutilizzabile.
- Senza spread De-Feedback crolla prima (8 istanze: 1678 persi) di EverClean (8: 0, 12: 1912).
- Il costo dell'hop non dipende dal segnale: con parlato vero è uguale a quello misurato col silenzio (De-Feedback 0.17 ms, EverClean 0.30 ms con 1 istanza).

```bash
$EXE --bench-device VST3 /Library/Audio/Plug-Ins/VST3/EverClean.vst3 8 32 Altoparlanti --multicore-only --set Active=On --input "/Users/bio/Documents/REAPER Media/Castello_21-5_Panel-4.wav" 600
```

### Nota su Dante Virtual Soundcard
Con DVS l'apertura del device può richiedere ~20 s (attesa dentro `coreaudiod`), e DVS dichiara già ~9-10 ms di round trip anche a 32 campioni. Con una scheda CoreAudio fisica la latenza totale scende davvero.

### Se `coreaudiod` non risponde
Un driver audio appena installato (successo con un driver HAL di rete) può tenere `coreaudiod` al 100% di CPU senza rispondere. La prima chiamata CoreAudio di un processo aspetta la risposta senza timeout, quindi fino alla 0.5.0 StageRack restava appeso **prima di aprire la finestra** e sembrava che non partisse. Fare la chiamata in un thread non basta: il thread tiene il lock interno della HAL e AppKit (il monitor App Nap del volume) chiama CoreAudio sul thread principale e si blocca lì dietro.
Ora la prima chiamata la fa un **processo figlio** (`StageRack --coreaudio-probe`): l'app non tocca CoreAudio finché il figlio non risponde. Se la risposta arriva entro 1.5 s l'avvio è identico a prima; altrimenti la finestra si apre comunque con l'avviso "Waiting for CoreAudio" (anche nella barra in alto) e il device e l'ultima sessione si aprono da soli appena `coreaudiod` risponde, a meno che nel frattempo tu non abbia aperto o modificato un'altra sessione. Mentre aspetta, SETTINGS spiega cosa succede invece di bloccarsi, e all'uscita il device salvato non viene cancellato. Rimedi: togliere il driver che blocca, oppure `sudo killall coreaudiod`.
Test: `STAGERACK_TEST_COREAUDIO_HANG=<secondi>` (0 = per sempre) simula un `coreaudiod` che non risponde.

### Istanze parcheggiate
De-Feedback 1.1.4 (Alpha Labs) può corrompere la memoria in rari casi quando un'istanza viene creata o distrutta mentre altre sono in funzione (crash "memory corruption of free block" visti in StageRack: in creazione nella libreria licenze del plugin, e in distruzione durante il re-spread). Per questo, a sessione aperta, StageRack **non distrugge** le istanze che togli (rimozione, sostituzione, rack cancellato, gemello sostituito dal re-spread): le **parcheggia** vive, ma fuori dal percorso audio, e le libera solo quando chiudi o cambi sessione, o all'uscita.
- Costo audio: zero, perché un'istanza parcheggiata non viene mai elaborata. Latenza: zero.
- Costo misurato: 12 istanze De-Feedback parcheggiate = 0,34% di un core in totale (fuori dal thread audio), circa 4,5 MB di RAM ciascuna.
- Il numero di istanze parcheggiate compare accanto allo stato del Re-spread (SETTINGS).
- **Pulizia automatica** (dalla 0.11.3): le istanze parcheggiate non restano più fino alla chiusura della sessione. Dopo 10 s di riposo (i loro thread sono fermi) StageRack libera, una alla volta, quelle che nessun Annulla/Ripeti può riportare indietro (per esempio i gemelli vecchi del Re-spread). Ne tiene al massimo **16** per l'Annulla: oltre, i passi di annullamento più vecchi vengono scartati e le loro istanze liberate. Motivo: in una sessione di 5 minuti con molti Re-spread se n'erano accumulate 87 di De-Feedback (777 MB, 271 thread).
- Stress test: `--stress-lifecycle VST3 <plugin> <istanze vive> <cicli> [--audio] [--keep] [--hold s]`. Crea e distrugge istanze mentre altre elaborano audio; si può lanciare anche con Guard Malloc (`DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib`). Risultato: 0 crash in 60 cicli con Guard Malloc e in 250 cicli normali. Il bug è raro e non si riproduce a comando.

### Stabilità dal vivo: dropout, crash, carico in background
**Confronto con la 0.5.1** (stesso Mac M4 Pro, Altoparlanti, 32 campioni, 0.5.1 ricompilata dal suo tag):

| De-Feedback | motore (`--bench-device`) 0.5.1 / attuale | app completa con interfaccia, 30 s, 0.5.1 / attuale |
|---|---|---|
| 4 | 0 / 0 persi, media 0.151 / 0.156 ms | — |
| 8 | 1 / 0 persi | 0 / 0 oltre budget, CPU processo ~145% in entrambe |
| 12 | 1 / 0 persi | 0 / 0 |
| 16 | 0 / 1 persi | 0 / 0, CPU ~260% in entrambe |

Il calcolo non è peggiorato: lo scheduling (worker, wake assist, HopAligner) è lo stesso codice della 0.5.1 e le funzioni nuove costano pochi µs per callback.

**Il problema vero era un crash**: con 12 De-Feedback a 16 campioni la versione attuale crashava in ~20 s (2 su 2), la 0.5.1 no. L'hop watch (dalla 0.6) crea un gemello per spostare un hop; se poi il gemello non serviva (hop già nella fase giusta) veniva **distrutto subito**, mentre i thread di calcolo di De-Feedback lavoravano ancora (heap danneggiato, crash in `cblas_sgemm`). Succedeva anche a ogni accensione di EverClean che collideva. Ora i gemelli non usati vengono parcheggiati come tutte le altre istanze: 5 run su 5 senza crash.

**Carico in background** (8-16 De-Feedback @32, misurato): 24 processi che saturano tutti i core, 48 thread a priorità user-interactive (come le app in primo piano), 12 thread Accelerate/AMX (stessa unità che usa De-Feedback): **0 callback persi**, anzi hop più veloci (core svegli, clock alto). Il motore e i thread di De-Feedback girano in tempo reale (priorità 97, sopra tutte le app) nel workgroup di CoreAudio. Restano fuori dal nostro controllo: Risparmio energetico, surriscaldamento, memoria piena, driver della scheda.

**Cosa fa l'app ora**:
- **Contatore dropout** sotto i meter DSP nella barra in alto: "0 dropouts" in verde, "N DROPOUTS · x s ago" in rosso. Conta i callback oltre budget, i callback **saltati** da CoreAudio (ricavati dagli orari dei buffer: il conteggio di overload di CoreAudio restava a 0 anche con callback saltati) e i blocchi muti mentre si modificano i rack. Riparte da 0 quando la sessione ha finito di caricarsi e allinearsi. Clic = azzera / mostra il log; tooltip = dettaglio.
- **`dropouts.log`** nella cartella dati: ora al millisecondo, tipo, rack più pesante in quel momento, stato del Mac. Utile per capire un click sentito durante lo show.
- **Avvisi di sistema** nella stessa riga: LOW POWER MODE, HOT/OVERHEATING, LOW MEMORY.
- **Attività "latency critical"** per tutta la durata dell'app: niente App Nap, timer precisi, niente sleep di sistema (lo schermo può spegnersi).

Per misurare un altro Mac (es. M3 base) dal Terminale, con l'app installata:
```bash
/Applications/StageRack.app/Contents/MacOS/StageRack --bench-device VST3 /Library/Audio/Plug-Ins/VST3/Defeedback.vst3 8 32 Speakers --multicore-only
```
(`Altoparlanti` su un Mac in italiano; `--threads` elenca i thread con priorità e CPU). Variabili per i test: `STAGERACK_DATA_DIR` = cartella dati separata (prove accanto a un'app aperta), `STAGERACK_WATCHLOG=1` stampa le decisioni dell'hop watch.

## Interfaccia leggera
Misurato con `sample`/`top` sull'M4 Pro, Altoparlanti a 16 campioni, finestra in primo piano, app ferma:

| | prima | dopo |
|---|---|---|
| Sessione nuova (0 rack): CPU StageRack | ~15% | ~5% |
| ZSP BIO (20 rack, 12 plugin): CPU StageRack | ~135-140% | ~63% (il resto è audio/plugin) |
| ZSP BIO: thread interfaccia | 95% (sempre occupato) | ~9% |
| ZSP BIO: WindowServer | ~41% | ~22% |

Cause: la ricerca dei parametri gemelli (sync) rifatta a ogni frame da righe parametro, slot e feedback MIDI (ora in cache per parametro, ricontrollata ogni 4 s); meter, info dei rack, CPU degli slot, TopBar e celle del tempo ridisegnati 30 volte al secondo anche senza cambiamenti (ora solo quando cambia ciò che si vede: meter a passi di 0.5 dB, testi 5 volte al secondo); feedback MIDI calcolato anche senza uscite MIDI aperte.
Caricamento di ZSP BIO: ~4.8 s dall'avvio, quasi tutto dentro i plugin (inizializzazione VST3 ~2.3 s, AU ~0.5 s) e l'avvio di CoreMIDI (~0.5 s).

## Solidità del thread audio
- **Grafo senza blocchi muti**: il grafo dei rack (catene, bus, send, stadi) ora si calcola sul thread interfaccia *fuori* dal lock e si scambia sotto il lock con pochi puntatori (`planGraph` / `applyGraphLocked`). Prima veniva calcolato sotto il lock: se il callback arrivava in quel momento saltava il blocco (blocco muto = click) a ogni aggiunta/rimozione di rack, cambio di uscita o send di un bus.
- **Attesa limitata invece di rinunciare** (`Engine/RtLock.h`): il callback aspetta il lock del grafo fino a 60 µs e quello di un rack fino a 40 µs (chi li tiene scambia solo puntatori); se il thread interfaccia fosse sospeso proprio lì, il blocco resta muto come prima e conta come dropout (anche quello di un singolo rack, prima invisibile).
- **Rilevatore di allocazioni** (`Engine/RtGuard`): con `--rt-guard` ogni malloc/free fatta dal *nostro* codice audio (callback, worker, rack, playhead) viene contata con lo stack; quelle dentro i plugin no. Si controlla da solo all'avvio (allocazione di prova vista / non vista).
- **Prova lunga** `--soak <sessione> [device] --seconds N --block B [--input wav] [--rt-guard] [--quiet-ui]`: sessione vera su scheda vera (uscita muta), un ingresso diverso per rack, e intanto ogni 100 ms un'azione da concerto (richiamo scena, parametri plugin, fader, mute, ricostruzione grafo, aggiunta/rimozione rack, snapshot undo). Report ogni 10 s.

Risultati (Altoparlanti, audio del Castello, rilevatore attivo):

| Sessione | Blocco | Durata | Dropout | Peggior callback | Attese lock | Allocazioni nostre |
|---|---|---|---|---|---|---|
| ZSP BIO (20 rack) | 32 | 60 s | 0 | 0.158 ms | 1 | 0 |
| Athesys brescia (28 plugin) | 32 | 60 s | 0 | 0.420 ms | 0 | 0 |
| StageRack-Test (con scene) | 32 | 60 s | 0 | 0.392 ms | 0 | 0 |
| etst2Bio03 | 32 | 60 s | 0 | 0.324 ms | 0 | 0 |
| ZSP BIO | 16 | 120 s | 0 | 0.109 ms | 2 | 0 |
| Athesys brescia | 16 | 120 s | 0 | 0.265 ms | 0 | 0 |

Le "attese lock" sono i casi in cui il codice vecchio avrebbe prodotto un blocco muto. Bench 8 e 12 De-Feedback dopo le modifiche: 0 callback oltre budget.

## Plugin isolati (sperimentale)
SETTINGS → Performance → *Plugin isolation (experimental)*, spento di default; all'accensione chiede conferma perché **costa molta CPU**. Vale per i plugin caricati da quel momento: per applicarlo a tutti si riapre la sessione. Negli slot compare **ISO** accanto al formato.
- Ogni plugin gira in un **processo suo** (lo stesso StageRack in modalità `--srk-sandbox`, senza icona nel Dock). Se crasha, si chiude solo quel processo: lo slot **lascia passare il suono dry**, mai silenzio, e il plugin **riparte da solo** con l'ultimo stato (aggiornato ogni 5 s e a ogni salvataggio o scena) in circa 0,5 s.
- **Zero latenza aggiunta**: l'audio passa in memoria condivisa nello stesso callback. Il rack aspetta il processo del plugin al massimo l'80% del blocco: se è in ritardo o bloccato quel blocco passa dry e il resto dell'audio va avanti.
- Le sessioni si salvano identiche, con lo stesso plugin e lo stesso stato: si possono riaprire con l'opzione spenta.
- La finestra del plugin la apre il suo processo; in StageRack resta un piccolo pannello con "Show plugin window". Niente miniature né meter del plugin negli slot isolati.

Misure:
- `--sandbox-test <formato> <plugin> [blocco]`. Uscita **identica bit per bit** al plugin dentro StageRack (AUDelay, De-Feedback). I parametri arrivano al plugin. Dopo il crash simulato lo slot passa dry e il plugin torna in 0,4-0,6 s, di nuovo identico bit per bit. Con il plugin bloccato 300 ms i blocchi passano dry, poi riprende.
- Passaggio tra processi: **~2 µs per blocco** di costo proprio (AUDelay); con De-Feedback il giro medio è ~48 µs, quasi tutto calcolo del plugin.
- ZSP BIO, 6 plugin isolati, scheda vera, azioni da concerto (`--soak ... --isolate`): **32 campioni 0 dropout, 0 blocchi dry**; 16 campioni 4 callback saltati nei primi 10 s (processi che si scaldano), poi 80 s puliti.
- **Costo**: CPU totale ~46% → ~76% con 6 plugin a 32 campioni (~5% di un core per plugin), peggior callback ~0.19 → ~0.49 ms. Su macOS 14.4+ il rack aspetta "dormendo" (`os_sync_wait_on_address`) dopo 20 µs; prima era ~+75%. Su macOS più vecchi gira a vuoto: costa di più.

Da usare per i plugin che si sa essere fragili, non per tutti: costa margine. Non provato: plugin che usano MIDI (non passa), sidechain e bus extra (disattivati come sempre), editor dei plugin dal vivo, slice Intel.
