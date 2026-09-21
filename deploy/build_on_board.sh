#!/bin/bash
# Build & deploy the embeded-web CGI system on the LubanCat board (Debian Buster aarch64).
# Mirrors README Phase 2; www assets are expected in /tmp/, source tarball extracts to /tmp/src.
set -e
cd /tmp
rm -rf src
tar xzf src.tar.gz
cd src
[ -f sqlite3.o ] || gcc -c -O2 -DSQLITE_THREADSAFE=0 sqlite3.c -o sqlite3.o
for src in login.cgi.c logout.cgi.c main.cgi.c network.cgi.c user_list.cgi.c user_create.cgi.c user_passwd.cgi.c user_toggle.cgi.c user_delete.cgi.c user_change_pass.cgi.c timesync.cgi.c ntpmon.cgi.c log.cgi.c; do
  name=$(echo "$src" | sed 's/\.cgi\.c//' | sed 's/\.c//').cgi
  gcc -Wall -O2 -o "$name" "$src" common.c auth.c gate.c users.c nmcli.c timesync.c ntpmon.c sha256.c sqlite3.o -lpthread -ldl
done
gcc -Wall -O2 -o db_init db_init.c auth.c gate.c common.c sha256.c sqlite3.o -lpthread -ldl
echo BUILD-OK
mkdir -p /var/db
./db_init admin
chown -R www-data:www-data /var/db
cp *.cgi /home/www/cgi-bin/
chown www-data:www-data /home/www/cgi-bin/*.cgi
chmod 755 /home/www/cgi-bin/*.cgi
cp /tmp/control_panel.html /tmp/index.html /tmp/style.css /tmp/change.html /tmp/uPlot.iife.min.js /tmp/uPlot.min.css /home/www/
chown www-data:www-data /home/www/*.html /home/www/style.css /home/www/uPlot*
echo DEPLOY-OK
ls -l /home/www/cgi-bin/
