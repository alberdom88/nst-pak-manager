# NST Pak Manager

App homebrew per Nintendo Switch (Atmosphère) che scarica file `.pak` da una cartella
remota (MEGA o il PC di casa) e li installa come mod di **Crash Bandicoot N. Sane
Trilogy**, con backup e ripristino. Può avviare il gioco direttamente dentro un livello,
come il tasto *Play* dell'editor sul PC, anche per i livelli nuovi con un nome qualsiasi.

I livelli fatti con Crash NST Maker sul PC sono nel formato della versione PC: prima
vanno convertiti per la Switch con il convertitore
[Crash-NST-Level-Editor-switch-conversion](https://github.com/alberdom88/Crash-NST-Level-Editor-switch-conversion)
(istruzioni in [`README_SWITCH.md`](https://github.com/alberdom88/Crash-NST-Level-Editor-switch-conversion/blob/main/README_SWITCH.md)).

- [Procedura completa](#procedura-completa)
- [Come funziona](#come-funziona)
- [Sorgenti](#sorgenti) · [Comandi](#comandi) · [Se il livello non funziona](#se-il-livello-non-funziona)
- [Strumenti per il PC](#strumenti-per-il-pc) · [Compilare](#compilare)

## Procedura completa

### Cosa serve

- Switch con **Atmosphère** e Crash Bandicoot N. Sane Trilogy (`0100D1B006744000`) con
  l'aggiornamento installato.
- PC con **Python 3** e l'emulatore **Eden** (serve per i dump del gioco e per provare i
  livelli prima di copiarli sulla Switch).
- Il convertitore `NST.exe` (Windows): si scarica dalla scheda **Actions** del
  [fork dell'editor](https://github.com/alberdom88/Crash-NST-Level-Editor-switch-conversion/actions),
  ultima esecuzione riuscita di *Build (Switch tools)* → artifact **NST-Switch-win-x64**.
- I **dump** del gioco fatti con Eden, che finiscono in
  `%APPDATA%\eden\dump\0100D1B006744000\`:
  - `romfs\` (i file del gioco): tasto destro sul gioco → **Dump RomFS** (con
    l'aggiornamento installato in Eden);
  - `exefs\` (l'eseguibile `main`): nelle impostazioni di Eden (Debug) attiva **Dump
    ExeFS**, avvia il gioco una volta e chiudilo.

### Preparazione (una volta sola)

1. **App.** Scarica l'artifact **nst-pak-manager-sd** dalla scheda Actions di questo
   repository (o compila, vedi [Compilare](#compilare)) e copia il contenuto nella radice
   della SD: l'app finisce in `sdmc:/switch/nst-pak-manager/`.
2. **Patch per l'avvio diretto e copia dell'`update.pak` originale.** Dal PC, nella cartella
   di questo repository:
   ```
   python tools\crea_avvio_livello.py "%APPDATA%\eden\dump\0100D1B006744000" --romfs "%APPDATA%\eden\dump\0100D1B006744000"
   ```
   Copia il contenuto di `avvio_livello\sd\` nella radice della SD. Contiene:
   - `atmosphere/exefs_patches/nst_avvio_livello/<ID build>.ips`: la patch (4 byte) che
     permette al gioco di leggere `debug.xml` ([perché](#avvio-diretto-nel-livello));
   - `switch/nst-pak-manager/originali/update.pak`: la copia dell'`update.pak` originale,
     su cui l'app costruisce la registrazione dei livelli nuovi.

   La patch vale per una versione precisa del gioco: se aggiorni il gioco, rifai i dump e
   questo passo.
3. **Configurazione.** Avvia l'app dall'Homebrew Menu: al primo avvio crea
   `sdmc:/switch/nst-pak-manager/config.json`. Aprilo dal PC (o via FTP), metti il link
   della tua cartella MEGA (vedi [Sorgenti](#sorgenti)) e riavvia l'app.
4. *(Facoltativo)* `originali.txt`, l'elenco dei `.pak` del gioco: serve solo per la
   scelta a mano dell'originale (tasto B) e per scrivere i nomi con le maiuscole giuste.
   ```
   python tools\list_originals.py "%APPDATA%\eden\dump\0100D1B006744000\romfs"
   ```
   e copia `originali.txt` in `sdmc:/switch/nst-pak-manager/`.

### Per ogni livello

1. **Crea il livello** con Crash NST Maker sul PC e salvalo: ottieni un `.pak` PC.
2. **Convertilo** con il convertitore
   ([dettagli](https://github.com/alberdom88/Crash-NST-Level-Editor-switch-conversion/blob/main/README_SWITCH.md)):
   ```
   NST.exe --switch converti "MioLivello.pak" "%APPDATA%\eden\dump\0100D1B006744000\romfs" "out\MioLivello.pak" "out\report.txt" --nuovo
   ```
   - `--nuovo`: il livello tiene il suo nome (consigliato). Si apre con l'avvio diretto,
     nei menu del gioco non compare.
   - `--come-originale`: per un livello creato nell'editor da uno originale
     (`L112_RoadToNowhere_Custom`): prende il posto dell'originale e si gioca anche dai menu.
   - `--sostituisci <livello>`: prende il posto del livello indicato (per esempio
     `L101_NSanityBeach`).

   L'archivio convertito prende il nome del livello (per esempio `Custom_Level.pak`).
3. **Caricalo** nella cartella MEGA (o nella cartella del PC). Basta il livello:
   `update.pak` lo crea l'app.
4. **Sulla Switch**: apri l'app, scheda REMOTI, premi **Y** sul livello. L'app lo scarica,
   lo installa con il suo nome, registra il livello se è nuovo e avvia il gioco già dentro.

Per tornare al gioco normale: **ZL** avvia il gioco dal menu e spegne l'avvio diretto;
**X** nella scheda INSTALLATI rimette gli originali.

### Provare un livello su Eden

Su Eden l'app non c'è: i file si mettono a mano nella cartella delle mod del gioco
(tasto destro sul gioco → cartella dei dati delle mod, di solito
`%APPDATA%\eden\load\0100D1B006744000\`). Ogni sottocartella è una mod:

```
load\0100D1B006744000\
├── NST avvio livello\          ← da avvio_livello\eden\ (patch e debug.xml)
│   ├── exefs\<ID build>.ips
│   └── romfs\debug.xml
└── Mio livello\
    └── romfs\archives\
        ├── Custom_Level.pak    ← il livello convertito
        └── update.pak          ← solo con --nuovo: quello creato dal convertitore
```

`debug.xml` deve nominare il livello da aprire: crealo indicando il `.pak` convertito.

```
python tools\crea_avvio_livello.py "%APPDATA%\eden\dump\0100D1B006744000" "out\Custom_Level.pak"
```

Tieni un solo `update.pak` tra le mod attive. Per tornare al gioco normale togli `debug.xml`.

## Come funziona

### Installazione e backup

I file originali del gioco stanno dentro il gioco stesso e **non vengono mai toccati**.
Atmosphère (LayeredFS) carica al loro posto i file che trova in:

```
sdmc:/atmosphere/contents/0100D1B006744000/romfs/archives/
```

L'app scrive lì i file scaricati. Se in quella cartella c'era già un file con lo stesso
nome (per esempio una mod installata a mano), prima lo sposta in
`sdmc:/switch/nst-pak-manager/backup/0100D1B006744000/`. **Ripristina** (X nella scheda
INSTALLATI) toglie il file installato dall'app e rimette quello salvato; se non c'era
nulla da salvare il gioco torna a usare il suo originale.

- Il download va prima in un file `.part`: se si interrompe o fallisce, i file esistenti
  restano come prima.
- Prima di installare l'app controlla dimensione e firma del file (`IGA\x1A`), così una
  pagina di errore o un link sbagliato non finisce nella cartella del gioco.
- Reinstallare un file già installato dall'app non sovrascrive il backup originale.
- Lo stato (installazioni e scelte) è in `sdmc:/switch/nst-pak-manager/state-0100D1B006744000.json`.

**Chiudi Crash prima di installare o ripristinare**: se il gioco è aperto in background
l'app mostra un avviso.

### Nome di installazione

Non serve dire all'app quale file sostituire: il nome lo prende dal file scaricato.

- **Livello**: viene installato con il nome del livello che ha dentro, per esempio
  `L112_RoadToNowhere.pak` o `Custom_Level.pak`, comunque si chiami su MEGA o sul PC. È
  l'unico nome con cui il gioco lo trova.
- **Altri file** (per esempio `update.pak`): l'ultima scelta fatta per quel file, poi il
  campo `target` del manifest, poi il nome stesso del file.

Per i casi particolari **B** apre l'elenco degli originali per scegliere a mano:

- il cursore parte dall'originale suggerito, segnato con `*`;
- **Y** apre la tastiera per cercare (es. `jungle`), **X** toglie il filtro;
- la prima voce permette di scrivere il nome a mano, la seconda di tenere il nome del file;
- accanto agli originali vedi `ora: <file>` se li hai già sostituiti con l'app, oppure
  `mod esterna` se nella cartella c'è un file messo a mano (finirà nel backup).

Un livello scelto a mano con un nome diverso dal suo viene comunque bloccato: il gioco
non lo troverebbe.

### Avvio diretto nel livello

**Y** nella scheda REMOTI, **Y** nella schermata del risultato dopo un'installazione o
**ZR** nella scheda INSTALLATI avviano il gioco già dentro il livello. L'app legge dal
`.pak` il nome del livello (per esempio `crash1/l112_roadtonowhere/l112_roadtonowhere`) e
lo scrive in `atmosphere/contents/0100D1B006744000/romfs/debug.xml`, il file di
configurazione di sviluppo che il gioco legge all'avvio:

```xml
<config>
	<MAP filename="crash1/l112_roadtonowhere/l112_roadtonowhere"/>
</config>
```

Con quel file il gioco carica il livello invece del menu, come con l'opzione `-om` del PC.
La versione in commercio però lo ignora: la funzione che ne permette la lettura
(`CConfigSystem::InFinal`) risponde sempre "no". La patch creata da
`tools/crea_avvio_livello.py` (passo 2 della preparazione) la fa rispondere "sì". Senza
`debug.xml` la patch non cambia nulla; se la patch manca, l'app lo segnala prima di avviare.

- L'avvio diretto resta attivo finché c'è `debug.xml`: anche avviando Crash dal menu HOME
  si entra nel livello. L'intestazione dell'app mostra `avvio diretto: ...`.
- **ZL** (o A nella schermata del risultato) avvia il gioco dal menu e cancella
  `debug.xml`. Anche il ripristino dell'originale di quel livello lo cancella.
- In più l'app passa al gioco l'opzione `-om <livello>` come argomento di avvio (servizio
  `ldr:shel` di Atmosphère). Il formato si cambia in `config.json` con `launch_args`; con
  `"launch_args": ""` non viene usata.

### Livelli nuovi (con il loro nome)

Un livello con un nome nuovo (per esempio `Custom_Level`) il gioco non lo conosce: va
registrato in `update.pak`, come fa l'editor sul PC quando premi *Play*. Il convertitore
con `--nuovo` mette i file della registrazione nell'archivio del livello (cartella interna
`update/`); quando avvii il livello, l'app li unisce alla copia dell'`update.pak` originale
(`sdmc:/switch/nst-pak-manager/originali/update.pak`) e installa il risultato come
`update.pak`, con il backup.

- Puoi tenere installati più livelli nuovi: a ogni avvio diretto l'app registra quello scelto.
- Nella scheda INSTALLATI l'`update.pak` creato compare come
  `update.pak <- registrazione di ...`: X lo toglie e rimette l'originale.
- Se manca la copia dell'`update.pak` originale, l'app lo dice e non avvia il gioco. Se il
  gioco non ha un `update.pak`, in `originali/` basta un file vuoto con quel nome.

## Sorgenti

`config.json` può contenere più sorgenti; nell'app si cambia con il tasto `-`.

### MEGA (`"type": "mega"`)

Gratuito: basta un account MEGA e il link della cartella, senza chiavi API.

1. Su [mega.nz](https://mega.nz) carica i `.pak` in una cartella.
2. Tasto destro sulla cartella → **Condividi** → **Copia link**. Il link deve contenere la
   chiave, cioè la parte dopo `#`.
3. Nella config:

```json
{ "name": "MEGA", "type": "mega", "url": "https://mega.nz/folder/AbCdEfGh#chiave-della-cartella" }
```

- L'app elenca i `.pak` che stanno direttamente nella cartella del link. Per una
  sottocartella aprila su mega.nz e copia il link dalla barra degli indirizzi
  (`.../folder/<cartella>#<chiave>/folder/<sottocartella>`).
- Su MEGA nomi e contenuti sono cifrati: l'app li decifra con la chiave del link. Chi ha
  il link completo può scaricare i file, quindi non pubblicarlo.
- Il traffico gratuito di MEGA ha una quota: se si esaurisce il download si ferma con
  "quota di trasferimento esaurita"; riprova più tardi.

### Cartella web (`"type": "http"`)

Va bene qualsiasi indirizzo che mostri l'elenco dei file (link ai `.pak`), oppure un
`manifest.json`. Il modo più semplice è il PC di casa, nella cartella con i `.pak`:

```
python -m http.server 8000
```

e nella config l'indirizzo IP del PC (su Windows lo trovi con `ipconfig`):

```json
{ "name": "PC di casa", "type": "http", "url": "http://192.168.1.50:8000/" }
```

Se Windows chiede il permesso del firewall, consenti l'accesso sulla rete privata.

Per un hosting che non mostra l'elenco della cartella, crea un manifest con
`python tools/make_manifest.py <cartella>`, caricalo insieme ai `.pak` e usa il suo URL.
Formato (`url`, `size` e `target` facoltativi; `url` può essere relativo; `target` è
l'originale proposto per i file che non sono livelli):

```json
{ "files": [ { "name": "MioLivello_v2.pak", "size": 123456789 },
             { "name": "update.pak", "url": "https://altro.sito/update.pak" } ] }
```

### Opzioni avanzate

| Campo         | Default                                          | A cosa serve |
|---------------|--------------------------------------------------|--------------|
| `title_id`    | `0100D1B006744000`                               | Gioco di destinazione |
| `mod_dir`     | `/atmosphere/contents/{title_id}/romfs/archives` | Cartella in cui installare |
| `ca_file`     | `cacert.pem` nella cartella dell'app (se esiste) | Certificati HTTPS aggiuntivi |
| `launch_args` | `nst -om {livello}`                              | Argomenti di avvio aggiuntivi per l'avvio diretto (vuoto = nessuno) |

Se una sorgente HTTPS dà errori di certificato, scarica `cacert.pem` da
<https://curl.se/docs/caextract.html> e mettilo in `sdmc:/switch/nst-pak-manager/`.

## Comandi

| Tasto | Scheda REMOTI | Scheda INSTALLATI |
|---|---|---|
| Su / Giù | sposta il cursore | sposta il cursore |
| Sinistra / Destra | pagina precedente / successiva | idem |
| A | seleziona / togli dalla selezione (nome automatico) | seleziona / deseleziona |
| Y | **gioca**: scarica il file (se serve), registra il livello se è nuovo e avvia il gioco dentro | seleziona tutto / niente |
| X | installa i selezionati (o quello sotto il cursore) | ripristina i selezionati |
| B | scegli a mano l'originale da sostituire | |
| ZR | ricarica l'elenco (e `originali.txt`) | avvia il gioco dentro il livello di quel file |
| L / R | vai a REMOTI / INSTALLATI | |
| - | cambia sorgente | |
| ZL | chiude l'app e avvia il gioco dal menu (spegne l'avvio diretto) | |
| + | esci | |
| B (durante il download) | annulla | |

Nella scheda REMOTI **I** indica un file già installato dall'app, con tra parentesi il
nome con cui è installato. Nella scheda INSTALLATI ogni riga mostra
`nome installato <- file scaricato`.

## Se il livello non funziona

- **Il gioco si apre dal menu invece che nel livello**: manca la patch (l'app lo segnala) o
  è per un'altra versione del gioco; rifai i dump e il passo 2 della preparazione.
- **"Livello nuovo non registrato"**: manca `switch/nst-pak-manager/originali/update.pak`.
- **Schermo nero o crash**: quasi sempre il `.pak` è ancora nel formato PC, o è stato
  rinominato. Rinominare il `.pak` non rinomina il livello che contiene (l'app installa
  comunque con il nome giusto e blocca le scelte a mano sbagliate). Il `.pak` si controlla
  dal PC con `tools/controlla_pak.py`, che lo confronta con gli originali del dump RomFS:

  ```
  python tools\controlla_pak.py MioLivello.pak --switch "%APPDATA%\eden\dump\0100D1B006744000\romfs"
  ```

  Il rapporto (`report_<nome>.txt`) mostra cosa cambia rispetto alla Switch (versione
  dell'archivio, percorsi, compressione, igz, Havok). Un `.pak` PC va convertito con il
  [convertitore](https://github.com/alberdom88/Crash-NST-Level-Editor-switch-conversion/blob/main/README_SWITCH.md),
  il cui rapporto di conversione dice anche cosa non è stato possibile convertire.

## Strumenti per il PC

Nella cartella `tools/` (Python 3, solo libreria standard):

| Script | A cosa serve |
|---|---|
| `crea_avvio_livello.py` | patch per l'avvio diretto, copia dell'`update.pak` originale (`--romfs`) e `debug.xml` per Eden |
| `list_originals.py` | crea `originali.txt` dal dump RomFS |
| `make_manifest.py` | crea `manifest.json` per un hosting senza elenco dei file |
| `controlla_pak.py` | confronta un `.pak` con gli originali Switch (formato, nomi, contenuto) |
| `analizza_avvio.py`, `cerca_opzioni.py` | analisi dell'eseguibile Switch (opzioni di avvio, configurazione) |
| `estrai_tipi.py`, `estrai_coppie.py`, `layout_switch.py` | strumenti usati per mettere a punto il convertitore |

## Compilare

### Con GitHub Actions (niente da installare)

Ogni push esegue il workflow *Build*: test su Linux e compilazione con devkitPro.
L'artifact **nst-pak-manager-sd** contiene la cartella da copiare nella radice della SD.

### In locale

Installa [devkitPro](https://devkitpro.org/wiki/Getting_Started) con il pacchetto
*Switch development* e la libreria curl:

```
dkp-pacman -S switch-dev switch-curl
make
```

(su Windows i comandi vanno nella shell MSYS2 di devkitPro, con `pacman` al posto di
`dkp-pacman`).

### Test su PC

```
sh tests/run_tests.sh
```

Compila la logica dell'app (download, installazione, backup, MEGA, archivi `.pak`) per
Linux (serve `libcurl4-openssl-dev`) e la prova contro un server locale che simula
cartella web, manifest e API di MEGA (con file cifrati come quelli veri). Gli `update.pak`
scritti dall'app vengono riletti e decompressi con un lettore indipendente in Python.

Si può provare anche l'interfaccia, con un `switch.h` finto che legge i tasti da uno
scenario e stampa le schermate:

```
python tests/ui_sim/ui_sim.py tests/ui_sim/scenari/completo.txt --config tests/ui_sim/sim_config.json
```

## Struttura

```
source/main.cpp   interfaccia (console libnx, solo Switch)
source/core.*     configurazione, sorgenti, installazione, backup, avvio diretto, registrazione
source/net.*      download con libcurl
source/mega.*     cartelle MEGA: elenco, AES e decifratura dei download
source/pak.*      archivi .pak: lettura e scrittura (update.pak dei livelli nuovi)
source/util.*     file, percorsi, URL
source/cJSON.*    parser JSON (MIT, vedi cJSON.LICENSE)
tests/            test su PC (tests/ui_sim: interfaccia simulata)
tools/            strumenti per il PC (vedi sopra)
```
