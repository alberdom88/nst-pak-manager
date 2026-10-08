# NST Pak Manager

App homebrew per Nintendo Switch (Atmosphère) per giocare i livelli personalizzati di
**Crash Bandicoot N. Sane Trilogy**. Scegli un livello da una cartella remota (MEGA o il PC
di casa) e premi **A**: l'app lo scarica, crea `update.pak` se serve, scrive `debug.xml` e
avvia il gioco già dentro il livello, come il tasto *Play* dell'editor sul PC.

I livelli fatti con Crash NST Maker sul PC sono nel formato della versione PC: prima
vanno convertiti per la Switch con il convertitore
[Crash-NST-Level-Editor-switch-conversion](https://github.com/alberdom88/Crash-NST-Level-Editor-switch-conversion)
(istruzioni in [`README_SWITCH.md`](https://github.com/alberdom88/Crash-NST-Level-Editor-switch-conversion/blob/main/README_SWITCH.md)).

- [Procedura completa](#procedura-completa)
- [Come funziona](#come-funziona)
- [Sorgenti](#sorgenti) · [Comandi](#comandi) · [Se il livello non funziona](#se-il-livello-non-funziona) · [Aggiornare dalla 1.8](#aggiornare-dalla-18)
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
2. **Patch per l'avvio diretto.** Dal PC, nella cartella di questo repository:
   ```
   python tools\crea_avvio_livello.py "%APPDATA%\eden\dump\0100D1B006744000" --romfs "%APPDATA%\eden\dump\0100D1B006744000"
   ```
   Copia il contenuto di `avvio_livello\sd\` nella radice della SD. Contiene:
   - `atmosphere/exefs_patches/nst_avvio_livello/<ID build>.ips`: la patch (4 byte) che
     permette al gioco di leggere `debug.xml` ([perché](#avvio-diretto-nel-livello));
   - `switch/nst-pak-manager/originali/update.pak`: la copia dell'`update.pak` originale
     (`--romfs`), su cui l'app costruisce la registrazione dei livelli nuovi. La versione
     1.0.0 del gioco non ha un `update.pak`: in quel caso è un file vuoto, e l'app scrive un
     `update.pak` con la sola registrazione (lo fa anche se la copia manca).

   La patch vale per una versione precisa del gioco: se aggiorni il gioco, rifai i dump e
   questo passo.
3. **Configurazione.** Avvia l'app dall'Homebrew Menu: al primo avvio crea
   `sdmc:/switch/nst-pak-manager/config.json`. Aprilo dal PC (o via FTP), metti il link
   della tua cartella MEGA (vedi [Sorgenti](#sorgenti)) e riavvia l'app.

### Per ogni livello

1. **Crea il livello** con Crash NST Maker sul PC e salvalo: ottieni un `.pak` PC.
2. **Convertilo** con il convertitore
   ([dettagli](https://github.com/alberdom88/Crash-NST-Level-Editor-switch-conversion/blob/main/README_SWITCH.md)):
   ```
   NST.exe --switch converti "MioLivello.pak" "%APPDATA%\eden\dump\0100D1B006744000\romfs" "out\MioLivello.pak" "out\report.txt" --nuovo
   ```
   - `--nuovo` (consigliato): il livello prende il nome del file di uscita, qui `MioLivello`,
     qualunque nome avesse nell'editor. Si apre con l'avvio diretto, nei menu del gioco non
     compare.
   - `--come-originale`: per un livello creato nell'editor da uno originale
     (`L112_RoadToNowhere_Custom`): prende il posto dell'originale e si gioca anche dai menu.
   - `--sostituisci <livello>`: prende il posto del livello indicato (per esempio
     `L101_NSanityBeach`).

   L'archivio viene scritto con il nome del livello in minuscolo (`out\miolivello.pak`):
   sulla Switch il gioco cerca i livelli solo così.
3. **Caricalo** nella cartella MEGA (o nella cartella del PC). Basta il livello:
   `update.pak` lo crea l'app.
4. **Sulla Switch**: apri l'app e premi **A** sul livello. L'app lo scarica, crea
   `update.pak` se il livello è nuovo, scrive `debug.xml` e avvia il gioco già dentro.

Per tornare al gioco normale: **ZL** toglie `debug.xml` e avvia il gioco dal menu.

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
        ├── miolivello.pak      ← il livello convertito (nome in minuscolo)
        └── update.pak          ← solo con --nuovo: quello creato dal convertitore
```

`debug.xml` deve nominare il livello da aprire: crealo indicando il `.pak` convertito.

```
python tools\crea_avvio_livello.py "%APPDATA%\eden\dump\0100D1B006744000" "out\miolivello.pak"
```

Il nome del file conta: il gioco apre `archives/<livello in minuscolo>.pak` e la romfs
distingue maiuscole e minuscole (`Custom_Level.pak` non viene trovato e il gioco resta sulla
schermata di caricamento). Lo script avvisa se il nome non è quello giusto.

Tieni un solo `update.pak` tra le mod attive. Per tornare al gioco normale togli `debug.xml`.

## Come funziona

### Cosa fa A

1. **Scarica sempre il livello**, anche se sulla SD ce n'è già uno con lo stesso nome. Il
   download va prima in un file `.part`; l'app controlla dimensione e firma del file
   (`IGA\x1A`), così una pagina di errore o un link sbagliato non finisce nella cartella
   del gioco. **B** durante il download annulla.
2. **Installa il livello** in `sdmc:/atmosphere/contents/0100D1B006744000/romfs/archives/`,
   dove Atmosphère (LayeredFS) lo carica al posto dei file del gioco, che non vengono mai
   toccati. Il nome è quello del livello che il file contiene, in minuscolo come tutti gli
   archivi della Switch: `custom_level.pak` per il livello `Custom_Level`,
   `l112_roadtonowhere.pak` per un livello che sostituisce Road to Nowhere, comunque si
   chiami il file su MEGA o sul PC. È l'unico nome con cui il gioco lo trova. Un `.pak` che
   non contiene un livello viene rifiutato.
3. **Crea `update.pak`** se il livello è nuovo (vedi sotto); se non lo è, toglie
   l'`update.pak` lasciato da un livello nuovo giocato prima.
4. **Scrive `debug.xml`** con il livello da aprire e **avvia il gioco**.

Si gioca **un livello alla volta**: il livello installato la volta prima viene tolto. Se
sostituiva un livello originale, il gioco torna a usare l'originale. L'ultimo livello e il
gioco scelto sono in `sdmc:/switch/nst-pak-manager/stato-0100D1B006744000.json`.

**Chiudi Crash prima di giocare un livello**: se il gioco è aperto in background l'app
mostra un avviso e non scarica nulla.

### Avvio diretto nel livello

L'app legge dal `.pak` l'identificativo del livello (per esempio
`crash1/l112_roadtonowhere/l112_roadtonowhere`) e lo scrive in
`atmosphere/contents/0100D1B006744000/romfs/debug.xml`, il file di configurazione di
sviluppo che il gioco legge all'avvio:

```xml
<config>
	<MAP filename="crash1/l112_roadtonowhere/l112_roadtonowhere"/>
</config>
```

Con quel file il gioco carica il livello invece del menu, come con l'opzione `-om` del PC.
La versione in commercio però lo ignora: la funzione che ne permette la lettura
(`CConfigSystem::InFinal`) risponde sempre "no". La patch creata da
`tools/crea_avvio_livello.py` (passo 2 della preparazione) la fa rispondere "sì". Senza
`debug.xml` la patch non cambia nulla; se la patch manca, l'app lo segnala.

- **Gioco (L/R)**: la prima parte dell'identificativo indica il gioco del livello. Con
  **Auto** (predefinito) l'app usa quella scritta nel livello. Con **Crash 1**, **Crash 2**
  o **Crash 3** la sostituisce in `debug.xml` (`crash3/...` diventa `crash1/...`); il
  livello resta com'è, quindi se il gioco scelto non è il suo il gioco potrebbe non
  trovarlo (l'app lo segnala). Il gioco di un livello si decide nell'editor e nel
  convertitore.
- L'avvio diretto resta attivo finché c'è `debug.xml`: anche avviando Crash dal menu HOME
  si entra nel livello. L'intestazione dell'app mostra `debug.xml: ...`.
- **ZL** cancella `debug.xml` e avvia il gioco dal menu.
- In più l'app passa al gioco l'opzione `-om <livello>` come argomento di avvio (servizio
  `ldr:shel` di Atmosphère). Il formato si cambia in `config.json` con `launch_args`; con
  `"launch_args": ""` non viene usata.

### Livelli nuovi (update.pak)

Un livello con un nome nuovo (per esempio `Custom_Level`) il gioco non lo conosce: va
registrato in `update.pak`, come fa l'editor sul PC quando premi *Play*. Il convertitore
con `--nuovo` mette i file della registrazione nell'archivio del livello (cartella interna
`update/`). L'app li mette in `update.pak`, uniti alla copia dell'`update.pak` originale
(`sdmc:/switch/nst-pak-manager/originali/update.pak`) se c'è e non è vuota; altrimenti
`update.pak` contiene solo la registrazione, come serve alla versione 1.0.0 del gioco, che
non ne ha uno.

L'app considera suo l'`update.pak` nella cartella delle mod: lo sovrascrive per un livello
nuovo e lo toglie per un livello che non ne ha bisogno.

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
Formato (`url` e `size` facoltativi; `url` può essere relativo):

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

| Tasto | Azione |
|---|---|
| Su / Giù | sposta il cursore |
| Sinistra / Destra | pagina precedente / successiva |
| A | **gioca**: scarica il livello, crea `update.pak` se serve, scrive `debug.xml` e avvia il gioco |
| L / R | gioco per `debug.xml`: Auto, Crash 1, Crash 2, Crash 3 |
| ZR | ricarica l'elenco |
| - | cambia sorgente |
| ZL | toglie `debug.xml` e avvia il gioco dal menu |
| + | esci |
| B (durante il download) | annulla |

## Se il livello non funziona

- **Il gioco si apre dal menu invece che nel livello**: manca la patch (l'app lo segnala) o
  è per un'altra versione del gioco; rifai i dump e il passo 2 della preparazione. Se hai
  scelto a mano un gioco diverso da quello del livello (L/R), torna ad **Auto**.
- **Il gioco resta sulla schermata di caricamento**, nell'ordine:
  1. nome del file con le maiuscole: il livello deve chiamarsi come dentro, in minuscolo
     (`custom_level.pak`). L'app lo installa sempre così; su Eden rinominalo a mano;
  2. versione del gioco diversa da quella del dump: la copia in `originali/update.pak` e
     i file di registrazione del livello vengono dal dump, quindi devono essere della stessa
     versione installata sulla Switch (HOME → icona di Crash → **+**, la versione è in alto);
  3. il livello stesso: provalo su Eden con gli stessi file. Se si blocca anche lì, allega il
     rapporto della conversione.
- **"il file non contiene un livello"**: il `.pak` non ha il pacchetto di un livello
  (`packages/generated/maps/...`): l'app serve solo per i livelli.
- **"copia dell'update.pak originale illeggibile"**: rifai la copia con il passo 2 della
  preparazione, oppure cancella `switch/nst-pak-manager/originali/update.pak` se il gioco è
  la versione 1.0.0.
- **Schermo nero o crash**: quasi sempre il `.pak` è ancora nel formato PC, o è stato
  rinominato. Rinominare il `.pak` non rinomina il livello che contiene (l'app installa
  comunque con il nome giusto). Il `.pak` si controlla
  dal PC con `tools/controlla_pak.py`, che lo confronta con gli originali del dump RomFS:

  ```
  python tools\controlla_pak.py MioLivello.pak --switch "%APPDATA%\eden\dump\0100D1B006744000\romfs"
  ```

  Il rapporto (`report_<nome>.txt`) mostra cosa cambia rispetto alla Switch (versione
  dell'archivio, percorsi, compressione, igz, Havok). Un `.pak` PC va convertito con il
  [convertitore](https://github.com/alberdom88/Crash-NST-Level-Editor-switch-conversion/blob/main/README_SWITCH.md),
  il cui rapporto di conversione dice anche cosa non è stato possibile convertire.

## Aggiornare dalla 1.8

La 2.0 non usa più installazioni, backup e `originali.txt` delle versioni precedenti.
I file installati dalla 1.8 restano nella cartella delle mod finché non giochi un altro
livello con la 2.0 (che toglie `update.pak` e i livelli con lo stesso nome) o non li
cancelli a mano. Le eventuali mod salvate dalla 1.8 sono ancora in
`sdmc:/switch/nst-pak-manager/backup/`.

## Strumenti per il PC

Nella cartella `tools/` (Python 3, solo libreria standard):

| Script | A cosa serve |
|---|---|
| `crea_avvio_livello.py` | patch per l'avvio diretto, copia dell'`update.pak` originale (`--romfs`) e `debug.xml` per Eden |
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

Compila la logica dell'app (download, preparazione del livello, MEGA, archivi `.pak`) per
Linux (serve `libcurl4-openssl-dev`) e la prova contro un server locale che simula
cartella web, manifest e API di MEGA (con file cifrati come quelli veri). Gli `update.pak`
scritti dall'app vengono riletti e decompressi con un lettore indipendente in Python.

Si può provare anche l'interfaccia, con un `switch.h` finto che legge i tasti da uno
scenario e stampa le schermate:

```
python tests/ui_sim/ui_sim.py tests/ui_sim/scenari/gioca.txt --config tests/ui_sim/sim_config.json
```

## Struttura

```
source/main.cpp   interfaccia (console libnx, solo Switch)
source/core.*     configurazione, sorgenti, preparazione del livello, update.pak, debug.xml
source/net.*      download con libcurl
source/mega.*     cartelle MEGA: elenco, AES e decifratura dei download
source/pak.*      archivi .pak: lettura e scrittura (update.pak dei livelli nuovi)
source/util.*     file, percorsi, URL
source/cJSON.*    parser JSON (MIT, vedi cJSON.LICENSE)
tests/            test su PC (tests/ui_sim: interfaccia simulata)
tools/            strumenti per il PC (vedi sopra)
```
