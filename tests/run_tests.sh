#!/bin/sh
# Compila ed esegue i test del core su Linux.
# Uso: tests/run_tests.sh [cartella_header_curl]
set -e
cd "$(dirname "$0")/.."
CURL_INC=${1:-/usr/include}
WORK=$(mktemp -d)
PORT=18765

gcc -c -O1 -o "$WORK/cJSON.o" source/cJSON.c
g++ -std=gnu++17 -O1 -Wall -Wextra -fno-exceptions -fno-rtti -I"$CURL_INC" \
    -o "$WORK/test_core" tests/test_core.cpp source/core.cpp source/mega.cpp source/pak.cpp source/net.cpp source/util.cpp "$WORK/cJSON.o" \
    $(ls /usr/lib/*/libcurl.so.4 2>/dev/null | head -1 || echo -lcurl)

mkdir -p "$WORK/www" "$WORK/sd"
python3 tests/crea_pak_prova.py "$WORK/pak"
# livello nuovo servito dal server di prova (registrazione unita a base_update.pak)
mkdir -p "$WORK/www/reg" && cp "$WORK/pak/Custom_Level.pak" "$WORK/www/reg/"
python3 tests/server.py "$WORK/www" $PORT &
SERVER=$!
trap 'kill $SERVER 2>/dev/null; rm -rf "$WORK"' EXIT
sleep 1
"$WORK/test_core" "http://127.0.0.1:$PORT" "$WORK/sd" "$WORK/pak"
# archivi scritti da pak.cpp: riletti e decompressi con il lettore Python
python3 tests/verifica_pak.py "$WORK/pak/riscritto.pak" base
python3 tests/verifica_pak.py "$WORK/pak/unito.pak"
