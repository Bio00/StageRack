# Novità di StageRack

Ogni versione ha la sua sezione `## versione — data`. L'app mostra queste note da sola dopo un aggiornamento (e dal menu StageRack → What's New...); `scripts/release.sh` usa la sezione della versione come note della release su GitHub.

## 0.8.1-beta — 2026-11-17

### Comandi
- **⌥-click = valore di default** su qualsiasi controllo: gain, IN, send (→ off), parametri dei plugin sugli slot, Weight dell'automixer. Premi e rilascia senza muovere; sui rack selezionati vale per tutti. ⌥-trascina resta "tutti i rack selezionati".
- **⌘ mentre trascini = regolazione fine** (5× più lenta) su tutti questi controlli, anche premuto o lasciato a metà trascinamento senza salti (⇧ continua a funzionare).
- **Dimensione di più rack insieme**: trascinando il bordo con **⌥** i rack selezionati prendono la stessa dimensione, con **⇧** cambiano della stessa quantità.

### Altro
- **SETTINGS → Changelog**: la storia completa delle versioni.
- Corretto: all'avvio il Dock mostrava una **seconda icona di StageRack** tra le app recenti (era il processo che controlla CoreAudio; anche scanner e probe delle latenze ora girano in background).

## 0.8.0-beta — 2026-11-17

### Tap tempo
- **Tempo master + fino a 8 tempi aggiuntivi**: pulsante del tempo nella barra in alto (nome, BPM, luce che segue la battuta), finestra Tempo (⌘T) e menu destro. I tempi nuovi seguono il master con un rapporto (×2, ÷2…) finché non li tappi tu.
- **Sorgenti**: tap col mouse o con la tastiera, **beat tracking dall'audio** (ingresso della scheda o un punto di un rack: prima dei plugin, dopo i plugin, dopo il fader), **MIDI clock** in ingresso, **OSC**.
- **Il tap vince sull'audio**: dopo un tuo tap l'audio corregge solo se è entro la tolleranza (±% regolabile); se dissente lo ignora, e se vuoi torna a seguirlo dopo N secondi.
- **I plugin si sincronizzano** al tempo del loro rack (delay, modulazioni): menu del rack → Tempo.
- **Uscita del tempo** (pulsante SEND): nota o CC MIDI a ogni battuta o a ogni tuo tap, MIDI clock, messaggi OSC (battuta e BPM) per luci e video.
- **OSC in ingresso** (porta 8000 di base, spento finché non lo accendi): `/stagerack/tap`, `/stagerack/bpm`, ×2/÷2…, oppure "impara" l'indirizzo che manda il tuo controller.
- **Lampeggio del tempo** regolabile in SETTINGS → TAP beat flash (da Off a Full).

### Bus, send e cartelle (stile Ableton)
- **Si accendono per sessione**: menu Rack → **Buses and Sends**. Spenti, la sessione suona esattamente come prima.
- **Bus A–H**: rack il cui ingresso è la somma di quello che gli mandano gli altri (rientri effetti, sottogruppi, master bus). Tasto destro su + ADD RACK → Add Bus, oppure ⌘⇧B.
- **Send** pre o post fader da ogni rack verso ogni bus, a manopole o a barre; le destinazioni che chiuderebbero un anello non compaiono.
- **Uscita verso un bus** (menu OUT → To bus) e **uscite multiple**: un rack può andare a più uscite della scheda e a più bus insieme (OUT → Additional outputs, oppure ⌃-click).
- **Cartelle** (⌘G): raggruppano rack sotto un bus, si chiudono e si aprono col triangolo, anche una dentro l'altra.
- Sempre **latenza zero**: tutto nello stesso callback. De-Feedback su un master bus con 8–12 tracce De-Feedback a 32 campioni: 0 callback persi.

### Vista e rack
- **VIEW** nella barra in alto: scegli quali sezioni mostrare in tutti i rack (I/O, ingresso, inserti, send, info) e quali bus nei send. Ogni rack può avere la sua vista (••• → Rack View).
- **Sezioni ridimensionabili** per rack: trascina i divisori fra inserti, send, ingresso e uscita. IN e OUT abbastanza grandi diventano **fader verticali** con scala in dB.
- **Gruppi di modifica** nella loro finestra (pulsante GROUPS, menu Rack → Edit Groups...).
- **Nuova icona** dell'app.

### Correzioni
- **Meter rossi bloccati**: un blocco con valori non validi (inf/NaN) dalla scheda o da un plugin ora viene azzerato invece di bloccare meter e audio del rack.
- **Finestra bloccata per minuti** con coreaudiod lento (Dante, dopo una chiusura brusca): la barra in alto non interroga più la scheda a ogni ridisegno.
- **Solo**: ora si sentono anche i rack collegati a valle (catene "From rack", bus).

## 0.7.0-beta — 2026-11-16

### Novità
- **Annulla / Ripeti (⌘Z / ⌘⇧Z)**: rack, plugin, I/O, gain, mute, gruppi, scene. L'annullamento non ricarica i plugin: le istanze tornano al loro posto all'istante.
- **Barra dei menu macOS** (File, Edit, Rack, Scene, Audio, View) con tutte le scorciatoie.
- **Gain In**: manopola IN all'inizio del rack (−24…+24 dB, prima dei plugin), con il meter d'ingresso accanto.
- **MUTE ALL**: mute generale di tutte le uscite (⌘⇧M), lampeggia quando è attivo.
- **Edit group stile Pro Tools** (menu ••• → Edit group, A–H): i rack del gruppo seguono gain (in relativo), M/S/B, on/off e parametri dei plugin, anche se mossi nella finestra del plugin, e l'inserimento/rimozione dei plugin. Un gruppo non cambia mai IN/OUT. Un rack in un gruppo attivo è incorniciato nel colore del gruppo. Click sul badge o sulle lettere A–H in alto per sospenderlo; ⌘⇧G per tutti.
- **⇧ come ⌥** sui rack selezionati: inserisci, togli e accendi/spegni i plugin su tutti in un colpo.
- **Rack colorati**: tutto il rack prende la tinta del suo colore. Ci sono 12 colori più Custom; per tornare ai rack grigi usa View → Tinted Racks.
- **Più rack in un colpo**: tasto destro su + ADD RACK, con ingressi consecutivi.
- **Barra in alto riorganizzata per funzione** (sessione, scene, live, audio, vista); resta tutta visibile anche con la finestra stretta.
- **Rack stretti leggibili**: layout ridotto con testi abbreviati.
- **Click a vuoto** fuori dai rack per togliere la selezione.

### Sicurezza dal vivo
- **La scheda audio resta quella scelta**: se sparisce (Dante fermato, USB staccata) StageRack non passa da solo ad altoparlanti e microfono del Mac. In alto compare "Waiting for…", e quando la scheda torna viene riaperta da sola. Altoparlanti e microfono integrati si usano solo se li scegli tu.

### Correzioni
- **Pro-Q 4 (CLAP)**: dopo aver toccato un parametro non mostra più la vista generica al posto della sua interfaccia.
- **I/O consecutivi (⌥⇧)**: ora funzionano con rack mono e stereo mescolati.
- **Rack stereo su una scheda con un solo ingresso** (per esempio il microfono del Mac): il meter e l'audio adesso funzionano.
- **Impostazioni**: il pulsante si chiama SETTINGS e nella finestra l'ingresso viene prima dell'uscita.

## 0.6.0-beta — 2026-11-16

**Novità: EverClean ottimizzato come De-Feedback, e controllo continuo degli hop.**

- **EverClean (Everworks)** ora è ottimizzato come De-Feedback. Quando lo inserisci da StageRack, *Active* viene acceso da solo (di base il plugin si carica spento e non fa nulla); poi il suo calcolo pesante, ogni 192 campioni, viene distribuito fra i callback come per De-Feedback. Latenza sempre 0. Sessioni, copie e scene mantengono lo stato salvato.
- **Controllo continuo degli hop**: durante lo show StageRack guarda dove ogni plugin "a blocchi" fa davvero il suo lavoro pesante. Se lo accendi dopo, lo togli dal bypass o richiami una scena e più istanze finiscono nello stesso callback, le ri-distribuisce **dal vivo, senza click** (stesso passaggio del Re-spread). Prova: 8 EverClean accesi tutti insieme si riordinano in ~13 s, senza callback persi durante il passaggio.
- **Core wake assist più intelligente**: si attiva solo per i plugin che durante il calcolo aspettano un loro thread interno (De-Feedback). Con EverClean non serve, quindi non consuma CPU.
- **Correzione**: il Re-spread dal vivo metteva il plugin nuovo 2 callback prima della posizione scelta. Ora va esattamente dove deve.

### Misure con audio reale (M4 Pro, parlato, un canale diverso per rack)
| | 32 campioni | 16 campioni |
|---|---|---|
| De-Feedback | 0 callback persi fino a 16 istanze | 0 persi fino a 12 istanze |
| EverClean | 0 callback persi fino a 16 istanze, **15-85% di CPU in meno** | 1-2 istanze al limite (qualche click), da 4 in su no |

EverClean calcola ogni blocco in un solo thread (~0.3 ms): a 16 campioni (budget 0.33 ms) non ci sta. **Con EverClean usa 32 campioni o più.** Tabelle complete nel README.

## 0.5.2-beta — 2026-11-16

**Fix: crash all'uscita.**

- **Uscendo con la finestra a schermo intero StageRack crashava sempre** (macOS "chiusura inaspettata"), e le impostazioni cambiate in quella sessione non venivano salvate. Corretto.
- **Uscita sicura con i plugin di terze parti**: all'uscita StageRack ora salva impostazioni, lista plugin e latenze, ferma l'audio, distrugge i plugin e termina subito. Prima, se un plugin aveva danneggiato la memoria, il crash poteva arrivare durante la pulizia finale (visto con sessioni con UA/PACE, Black Rooster, Plugin Alliance, EverClean).
- Nessun cambiamento all'audio, alla latenza o alle sessioni.

## 0.5.1-beta — 2026-11-11

**Fix: StageRack non si apriva quando `coreaudiod` non risponde.**

Un driver audio appena installato può tenere `coreaudiod` bloccato al 100% di CPU: la prima chiamata CoreAudio non ritornava mai e l'app restava appesa prima di mostrare la finestra.

- La prima chiamata a CoreAudio ora la fa un processo figlio: l'app non si blocca più.
- Se CoreAudio non risponde entro 1.5 s la finestra si apre comunque con l'avviso "Waiting for CoreAudio"; device e ultima sessione si aprono da soli appena risponde.
- Mentre aspetta, il device salvato non viene cancellato e AUDIO/DSP spiega cosa succede.
- Rimedio se dura: togliere il driver che blocca oppure `sudo killall coreaudiod`.

## 0.5.0-beta — 2026-11-11

Quinta beta di **StageRack**: **automixer integrato stile Dugan**, a latenza zero.

### Novità
- **Automixer (gain sharing)** per microfoni di parlato: conferenze, culto, teatro, panel. Ogni microfono del gruppo riceve la sua quota del guadagno totale: chi parla sale a 0 dB, gli altri scendono; nelle pause la sala resta al livello di un solo microfono aperto. Niente gate, niente soglie, niente tempi di attacco.
- **Come si usa**: menu del rack (••• o tasto destro) → **Automix** → Group A-D, per tutti i rack selezionati. Pulsante **AUTOMIX** nella barra in alto = on/off di tutti i gruppi. **Weight** per microfono (+6 … −20 dB). Si salva nella sessione e nelle scene.
- **Meter**: colonna **AM** su ogni rack (guadagno dell'automix, nel colore del gruppo) e finestra **AM METERS** con una striscia per microfono: ingresso, guadagno, weight, gruppo, mute.
- **Quando parlano in più**: *Share (Dugan)*, il guadagno si divide come nel Dugan (predefinito); *Soft*; *Open*, ogni persona che parla resta aperta. Con una persona sola e nelle pause tutte e tre si comportano come il Dugan.
- **Rilevatore**: ascolta solo la banda voce (rientri di basse e ronzii non prendono la parola), misura dopo il fader come Dugan Speech nelle console Waves, e chiude in circa 0,2 s un microfono che smette di parlare.
- **Latenza zero, CPU trascurabile**: il guadagno si applica dove i rack vengono sommati nelle uscite; circa 3 µs per callback con 32 rack.

### Confronto misurato
Scena parlata di 41 s con 4 voci, rientri tra microfoni e rumore di sala, fatta passare nel nostro automixer, in **Dugan Speech dentro SuperRack Performer** (via loopback) e in Everworks QUORUM:

| | StageRack (Share) | Dugan Speech | QUORUM |
|---|---|---|---|
| Parole abbassate con una persona sola | 0,1 % | 0,6 % | 0,2 % |
| Miglioramento parlato / (rientri + rumore) | 12,3 dB | 11,5 dB | 11,5 dB |
| Primi 20 ms di ogni frase | −4,2 dB | −5,3 dB | −7,1 dB |

Il nostro in modalità Share si comporta come il Dugan vero (che anche lui divide il guadagno quando parlano in due). Le voci di prova sono sintetiche: **provalo con i tuoi microfoni prima di uno spettacolo**. Tabella completa nel README.

## 0.4.0-beta — 2026-11-11

Quarta beta di **StageRack**: **niente più distruzione di plugin durante lo show**.

### Novità
- **Istanze parcheggiate**: a sessione aperta, i plugin che togli non vengono più distrutti. Vale quando rimuovi o sostituisci un plugin, cancelli un rack, o fai il Re-spread (che scarta il gemello vecchio). Le istanze tolte vengono **parcheggiate**: restano in memoria ma fuori dall'audio, e vengono liberate solo quando chiudi o cambi sessione, o all'uscita. Il loro numero compare accanto allo stato del Re-spread (AUDIO/DSP).
- **Costo**: latenza zero, CPU zero nel callback audio. Misurato: 12 istanze De-Feedback parcheggiate = 0,34% di un core in totale, fuori dal thread audio, e circa 4,5 MB di RAM ciascuna.
- Nuovo test diagnostico `--stress-lifecycle`: crea e distrugge istanze mentre altre elaborano audio, anche con Guard Malloc.

### Perché: Alpha Labs De-Feedback 1.1.4
Uno dei crash rari di De-Feedback (corruzione di memoria) è avvenuto mentre un'istanza veniva distrutta durante il Re-spread, con le altre in funzione. Ora durante lo show questo non succede più. Resta possibile, raramente, il crash alla **creazione** di un'istanza: carica i De-Feedback prima di andare live. In ogni caso l'autosalvataggio protegge il lavoro.

## 0.3.0-beta — 2026-11-08

Terza beta di **StageRack**: **autosalvataggio e recupero dopo un crash**.

### Novità
- **Autosalvataggio**: ogni 15 s, se la sessione è cambiata, StageRack ne salva una copia completa (rack, scene, stato dei plugin) in `~/Library/Application Support/StageRack/Autosave.stagerack`. Il tuo file `.stagerack` non viene toccato; nulla viene scritto mentre i plugin si caricano. Nessun effetto sull'audio.
- **Recupero dopo un crash**: se StageRack non si è chiuso normalmente, al riavvio chiede — prima di caricare i plugin — **Restore** (la sessione torna com'era, segnata come non salvata) oppure **Open last saved** (l'autosalvataggio resta da parte come `Autosave (not restored).stagerack`).

### Nota su Alpha Labs De-Feedback 1.1.4
Raramente l'app può chiudersi mentre si **aggiunge** un'istanza di De-Feedback quando altre sono già caricate (corruzione di memoria dentro l'inizializzazione del plugin, vista anche in test senza il resto di StageRack): è un problema del plugin. Consiglio: carica tutti i De-Feedback prima di andare live. Ora, se succede, non si perde il lavoro.

## 0.2.0-beta — 2026-11-08

Seconda beta di **StageRack**: arrivano gli **aggiornamenti automatici**.

### Novità
- **Aggiornamenti da GitHub**: all'avvio StageRack controlla se c'è una versione nuova e chiede se scaricarla e installarla (Download and install / Later / Skip this version). Controllo manuale dal menu StageRack → Check for Updates..., oppure AUDIO / DSP → Check now; lì si può anche disattivare il controllo all'avvio.
- Il download viene verificato (app, versione, firma) prima di installare. L'installazione chiede di salvare la sessione, sostituisce l'app nella stessa cartella e la riapre. L'audio si ferma per qualche secondo: non farlo durante uno spettacolo.

## 0.1.0-beta — 2026-11-08

Prima beta pubblica di **StageRack**, host di plugin per l'audio dal vivo.

### Novità di questa beta
- **⌥ (Alt) = modifica tutti i rack selezionati**: gain (⌥⇧ relativo), M/S/B, IN/OUT (⌥⇧ o ⌥⌘ = canali consecutivi dal canale scelto), accensione, vista e parametri dello stesso plugin.
- Corretto: con canali disattivati nel setup della scheda audio, gli I/O dei rack finivano sul canale sbagliato.

### Punti chiave
- Zero latenza aggiunta dal processing, multicore con Audio Workgroup di CoreAudio.
- Distribuzione automatica dei picchi di plugin a blocchi interni (es. Alpha Labs De-Feedback): 12 istanze a 32 campioni senza callback persi su M4 Pro.
- Scene con MIDI Program Change, rack concatenati, gruppi di compensazione della latenza, AU / VST3 / CLAP / LV2.
