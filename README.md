# NST Pak Manager

App homebrew per Nintendo Switch (Atmosphère) che scarica file `.pak` da una
cartella remota e li installa come mod di **Crash Bandicoot N. Sane Trilogy**,
con backup e ripristino.

Per ogni file scaricato scegli quale `.pak` originale del gioco deve sostituire:
il file remoto può chiamarsi come vuoi (es. `MioLivello_v2.pak`) e viene
installato con il nome dell'originale (es. `L101_NSanityBeach.pak`).

## Come funziona il backup

I file originali del gioco stanno dentro il gioco stesso e **non vengono mai
toccati**. Atmosphère (LayeredFS) carica al loro posto i file che trova in:

```
sdmc:/atmosphere/contents/0100D1B006744000/romfs/archives/
```

L'app scrive lì il file scaricato con il nome dell'originale scelto. Se in quella
cartella c'era già un file con quel nome (per esempio una mod installata a mano),
prima lo sposta in:

```
sdmc:/switch/nst-pak-manager/backup/0100D1B006744000/
```

**Ripristina** toglie il file installato dall'app e rimette quello salvato. Se
non c'era nulla da salvare, togliere il file basta: il gioco torna a usare il
suo originale.

Altre garanzie:
- il download va prima in un file `.part`: se si interrompe o fallisce, i file
  esistenti restano come prima;
- prima di installare controlla dimensione e firma del file (`IGA\x1A`), così
  una pagina di errore o un link sbagliato non finisce nella cartella del gioco;
- reinstallare un file già installato dall'app non sovrascrive il backup
  originale;
- lo stato (installazioni e abbinamenti scelti) è salvato in
  `sdmc:/switch/nst-pak-manager/state-0100D1B006744000.json`.

## Installazione sulla Switch

1. Compila l'app (vedi sotto) e copia `nst-pak-manager.nro` in
   `sdmc:/switch/nst-pak-manager/`.
2. Avvia l'app dall'Homebrew Menu. Al primo avvio crea
   `sdmc:/switch/nst-pak-manager/config.json` di esempio.
3. Modifica `config.json` dal PC (o via FTP) con le tue sorgenti.
4. Crea `originali.txt` (vedi sotto) e copialo nella stessa cartella. Poi riavvia.

**Chiudi Crash prima di installare o ripristinare.** Se il gioco è aperto in
background l'app mostra un avviso. Dopo l'installazione puoi avviare Crash
direttamente dall'app (A nella schermata del risultato, oppure ZL): il gioco
parte con i file appena installati, dal menu iniziale.

### Entrare direttamente nel livello

Come il tasto *Play* dell'editor sul PC, l'app può far partire il gioco già dentro
il livello installato: **Y** nella schermata del risultato, oppure **ZR** sulla
riga del file nella scheda INSTALLATI. L'app legge dal `.pak` il nome del livello
(per esempio `crash1/l112_roadtonowhere/l112_roadtonowhere`) e lo scrive in
`debug.xml`, il file di configurazione di sviluppo che il gioco legge all'avvio:

```xml
<config>
	<MAP filename="crash1/l112_roadtonowhere/l112_roadtonowhere"/>
</config>
```

Con quel file il gioco carica il livello invece del menu, come con l'opzione `-om`
del PC. Il file va in `atmosphere/contents/0100D1B006744000/romfs/debug.xml`.

**Patch (una volta sola).** La versione in commercio ignora `debug.xml`: la funzione
che ne permette la lettura (`CConfigSystem::InFinal`) risponde sempre "no". Una
patch IPS di 4 byte la fa rispondere "sì". La patch dipende dalla versione del
gioco, quindi si crea dal dump dell'eseguibile (ExeFS):

```
python tools/crea_avvio_livello.py "<cartella del dump ExeFS>" "<livello.pak>"
```

Lo script crea la cartella `avvio_livello` con:

- `sd/`: da copiare nella radice della SD. Mette la patch in
  `atmosphere/exefs_patches/nst_avvio_livello/<ID build>.ips` e un `debug.xml` di
  prova per il livello indicato.
- `eden/NST avvio livello/`: la stessa cosa come mod di Eden (cartella delle mod del
  gioco: tasto destro sul gioco, voce per aprire la cartella dei dati delle mod).

Senza `debug.xml` la patch non cambia nulla. Se la patch manca, l'app lo segnala
prima di avviare.

- L'avvio diretto resta attivo finché c'è `debug.xml`: anche se avvii Crash dal
  menu HOME entra nel livello. L'intestazione dell'app mostra `avvio diretto: ...`.
- **ZL** (o A nella schermata del risultato) avvia il gioco dal menu e cancella
  `debug.xml`. Anche il ripristino dell'originale di quel livello lo cancella.
- In più l'app passa al gioco l'opzione `-om <livello>` come argomento di avvio
  (servizio `ldr:shel` di Atmosphère), se disponibile. Il formato si cambia in
  `config.json` con `launch_args`; con `"launch_args": ""` non viene usata.

## Elenco degli originali (`originali.txt`)

Serve per proporti i nomi dei `.pak` del gioco quando scegli cosa sostituire. Si
crea una volta dal dump della RomFS (in Eden: tasto destro sul gioco → *Dump
RomFS*):

```
python tools/list_originals.py "<cartella del dump>"
```

Copia il file `originali.txt` creato in `sdmc:/switch/nst-pak-manager/`. È un
semplice file di testo, un nome per riga: puoi anche scriverlo o correggerlo a
mano. Senza questo file l'app funziona lo stesso, ma il nome va scritto con la
tastiera.

## Scegliere l'originale da sostituire

Nella scheda REMOTI premi **A** su un file: si apre l'elenco degli originali.

- il cursore parte dall'originale suggerito, segnato con `*`: quello scelto
  l'ultima volta per lo stesso file, oppure il campo `target` del manifest,
  oppure lo stesso nome se è tra gli originali;
- **Y** apre la tastiera per cercare (es. `jungle`), **X** toglie il filtro;
- la prima voce permette di scrivere il nome a mano, la seconda di tenere il
  nome del file scaricato;
- accanto agli originali vedi `ora: <file>` se li hai già sostituiti con l'app,
  oppure `mod esterna` se nella cartella c'è un file messo a mano (finirà nel backup).

Il file scelto appare come `MioLivello_v2.pak -> L101_NSanityBeach.pak`. Premi
di nuovo **A** per toglierlo dalla selezione. Se assegni un originale già
assegnato a un altro file, l'abbinamento precedente viene tolto, così due file
non finiscono mai sullo stesso nome. **Y** nella scheda REMOTI seleziona in un
colpo tutti i file che hanno un suggerimento.

## Se il livello resta nero o il gioco crasha

Le cause tipiche sono due. Si controllano dal PC con `tools/controlla_pak.py`,
che confronta il tuo `.pak` con gli originali Switch del dump RomFS (indica la
cartella del dump: lo script cerca da solo i file corrispondenti in tutti gli archivi):

```
python tools/controlla_pak.py MioLivello.pak --switch "<cartella del dump>" --come L101_NSanityBeach.pak
```

Stampa un riassunto e scrive il rapporto completo in `report_<nome>.txt`.

1. **Nome interno diverso.** Un livello tiene i suoi file in cartelle con il suo
   nome (`maps/Crash1/L101_NSanityBeach/...`). Rinominare il `.pak` non rinomina
   il contenuto: un livello creato nell'editor come `Custom_Level` e installato
   come `L101_NSanityBeach.pak` non viene trovato. L'app ora blocca questo caso
   e lascia tutto com'era. Per sostituire un livello, nell'editor apri quel
   livello originale e modificalo, così il nome interno resta giusto.
2. **Formato PC.** I `.pak` fatti con Crash NST Maker o presi da mod per PC sono
   nel formato della versione PC. Lo script mostra cosa cambia rispetto
   all'originale Switch (versione dell'archivio, percorsi, compressione,
   intestazione degli igz, formati di texture e vertici, Havok): se ci sono
   differenze, il file va convertito prima di usarlo sulla Switch.

## Sorgenti

`config.json` può contenere più sorgenti; nell'app si cambia con il tasto `-`.

### Cartella web (`"type": "http"`)

Va bene qualsiasi indirizzo che mostri l'elenco dei file (link ai `.pak`), oppure
un `manifest.json`.

Il modo più semplice è il PC di casa. Nella cartella con i `.pak`:

```
python -m http.server 8000
```

e nella config metti l'indirizzo IP del PC (su Windows lo trovi con `ipconfig`):

```json
{ "name": "PC di casa", "type": "http", "url": "http://192.168.1.50:8000/" }
```

Se Windows chiede il permesso del firewall, consenti l'accesso sulla rete privata.

Per un hosting che non mostra l'elenco della cartella, genera un manifest con
`python tools/make_manifest.py <cartella>`, caricalo insieme ai `.pak` e usa il
suo URL:

```json
{ "name": "Hosting", "type": "http", "url": "https://esempio.it/pak/manifest.json" }
```

Formato del manifest (`url`, `size` e `target` facoltativi; `url` può essere
relativo; `target` è l'originale proposto per quel file):

```json
{ "files": [ { "name": "MioLivello_v2.pak", "size": 123456789, "target": "L101_NSanityBeach.pak" },
             { "name": "update.pak", "url": "https://altro.sito/update.pak" } ] }
```

### MEGA (`"type": "mega"`)

Gratuito: basta un account MEGA (20 GB) e il link della cartella, senza chiavi API.

1. Su [mega.nz](https://mega.nz) carica i `.pak` in una cartella.
2. Tasto destro sulla cartella → **Condividi** → **Copia link**. Il link deve
   contenere la chiave, cioè la parte dopo `#`. Non va bene il link "senza chiave".
3. Nella config:

```json
{
  "name": "MEGA",
  "type": "mega",
  "url": "https://mega.nz/folder/AbCdEfGh#chiave-della-cartella"
}
```

- L'app elenca i `.pak` che stanno direttamente nella cartella del link. Per usare
  una sottocartella, aprila su mega.nz e copia il link dalla barra degli indirizzi
  (`.../folder/<cartella>#<chiave>/folder/<sottocartella>`).
- Su MEGA nomi e contenuti dei file sono cifrati: l'app li decifra con la chiave
  del link. Chi ha il link completo può scaricare i file, quindi non pubblicarlo.
- Il traffico gratuito di MEGA ha una quota (alcuni GB ogni qualche ora). Se si
  esaurisce, il download si ferma con "quota di trasferimento esaurita": riprova
  più tardi.

### Opzioni avanzate

| Campo       | Default                                          | A cosa serve |
|-------------|--------------------------------------------------|--------------|
| `title_id`  | `0100D1B006744000`                               | Gioco di destinazione |
| `mod_dir`   | `/atmosphere/contents/{title_id}/romfs/archives` | Cartella in cui installare |
| `ca_file`   | `cacert.pem` nella cartella dell'app (se esiste) | Certificati HTTPS aggiuntivi |
| `launch_args` | `nst -om {livello}`                            | Argomenti di avvio aggiuntivi per l'avvio diretto (vuoto = nessuno) |

Se una sorgente HTTPS dà errori di certificato, scarica `cacert.pem` da
<https://curl.se/docs/caextract.html> e mettilo in `sdmc:/switch/nst-pak-manager/`.

## Comandi

| Tasto | Scheda REMOTI | Scheda INSTALLATI |
|---|---|---|
| Su / Giù | sposta il cursore | sposta il cursore |
| Sinistra / Destra | pagina precedente / successiva | idem |
| A | scegli l'originale da sostituire / togli dalla selezione | seleziona / deseleziona |
| Y | seleziona i file con un suggerimento / azzera | seleziona tutto / niente |
| X | installa i selezionati (o sceglie per quello sotto il cursore) | ripristina i selezionati |
| ZR | ricarica l'elenco (e `originali.txt`) | avvia il gioco dentro il livello di quel file |
| L / R | vai a REMOTI / INSTALLATI | |
| - | cambia sorgente | |
| ZL | chiude l'app e avvia il gioco dal menu (spegne l'avvio diretto) | |
| + | esci | |
| B (durante il download) | annulla | |

Nella scheda REMOTI **I** indica un file già installato dall'app, con tra
parentesi l'originale che sostituisce. Nella scheda INSTALLATI ogni riga mostra
`originale <- file scaricato`.

## Compilare

### Con GitHub Actions (niente da installare)

1. Crea un repository su GitHub (anche privato) e carica questa cartella.
2. Nella scheda **Actions** parte il workflow *Build*: esegue i test e compila.
3. Scarica l'artifact **nst-pak-manager-sd** e copia il contenuto nella radice
   della SD.

### In locale

Installa [devkitPro](https://devkitpro.org/wiki/Getting_Started) con il pacchetto
*Switch development* e la libreria curl:

```
dkp-pacman -S switch-dev switch-curl
make
```

(su Windows i comandi vanno nella shell MSYS2 di devkitPro, con `pacman` al
posto di `dkp-pacman`).

### Test della logica su PC

```
sh tests/run_tests.sh
```

Compila il codice di download/installazione/backup per Linux (serve
`libcurl4-openssl-dev`) e lo prova contro un server locale che simula cartella
web, manifest e API di MEGA (con file cifrati come quelli veri).

Si può provare anche l'interfaccia sul PC, con un `switch.h` finto che legge i
tasti da uno scenario e stampa le schermate:

```
python tests/ui_sim/ui_sim.py tests/ui_sim/scenari/completo.txt --config tests/ui_sim/sim_config.json
```

## Struttura

```
source/main.cpp   interfaccia (console libnx, solo Switch)
source/core.*     configurazione, sorgenti, installazione, backup, ripristino
source/net.*      download con libcurl
source/mega.*     cartelle MEGA: elenco, AES e decifratura dei download
source/util.*     file, percorsi, URL
source/cJSON.*    parser JSON (MIT, vedi cJSON.LICENSE)
tests/            test su PC (tests/ui_sim: interfaccia simulata)
tools/            controlla_pak.py, list_originals.py, make_manifest.py, crea_avvio_livello.py
```
