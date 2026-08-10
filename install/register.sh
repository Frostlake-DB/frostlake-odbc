#!/bin/bash
#
# Register the Frostlake ODBC driver and a default DSN for the CURRENT USER —
# no root, nothing under /etc. unixODBC reads ~/.odbcinst.ini and ~/.odbc.ini
# after the system files, so a per-user entry is all a normal setup needs.
#
# Usage:
#   install/register.sh                          driver + DSN "Frostlake" (localhost:18082)
#   install/register.sh --port 18095             ... different port
#   install/register.sh --dsn NAME --database DB  ... different DSN name / start database
#   install/register.sh --remove                 take both entries out again
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIB="$HERE/build/libfrostlakeodbc.so"
DRIVER_NAME="Frostlake ODBC Driver"
DSN="Frostlake"
SERVER="localhost"
PORT="18082"
DATABASE=""
SCHEMA=""
REMOVE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --dsn) DSN="$2"; shift 2 ;;
        --server) SERVER="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --database) DATABASE="$2"; shift 2 ;;
        --schema) SCHEMA="$2"; shift 2 ;;
        --remove) REMOVE=1; shift ;;
        *) echo "unknown option: $1"; exit 1 ;;
    esac
done

ODBCINST="${ODBCINSTINI:-$HOME/.odbcinst.ini}"
ODBCINI="${ODBCINI:-$HOME/.odbc.ini}"

# Remove an ini section (["name"] up to the next section) in place.
strip_section() {
    local file="$1" name="$2"
    [ -f "$file" ] || return 0
    python3 - "$file" "$name" <<'PY'
import re, sys
path, name = sys.argv[1], sys.argv[2]
text = open(path).read()
pattern = re.compile(r'(?ms)^\[' + re.escape(name) + r'\][^\[]*')
open(path, 'w').write(pattern.sub('', text).lstrip('\n'))
PY
}

strip_section "$ODBCINST" "$DRIVER_NAME"
strip_section "$ODBCINI" "$DSN"

if [ "$REMOVE" = 1 ]; then
    echo "removed driver '$DRIVER_NAME' and DSN '$DSN' from $ODBCINST / $ODBCINI"
    exit 0
fi

[ -f "$LIB" ] || { echo "driver not built — run make first ($LIB missing)"; exit 1; }

cat >> "$ODBCINST" <<EOF
[$DRIVER_NAME]
Description = ODBC driver for the Frostlake SQL engine (HTTP transport)
Driver      = $LIB
Threading   = 2

EOF

{
    echo "[$DSN]"
    echo "Driver      = $DRIVER_NAME"
    echo "Description = Frostlake SQL engine at $SERVER:$PORT"
    echo "Server      = $SERVER"
    echo "Port        = $PORT"
    [ -n "$DATABASE" ] && echo "Database    = $DATABASE"
    [ -n "$SCHEMA" ] && echo "Schema      = $SCHEMA"
    echo
} >> "$ODBCINI"

echo "registered driver '$DRIVER_NAME' ($LIB)"
echo "registered DSN '$DSN' -> $SERVER:$PORT${DATABASE:+ database=$DATABASE}"
echo
echo "try it:   isql -v '$DSN'"
echo "DSN-less: 'Driver=$DRIVER_NAME;Server=$SERVER;Port=$PORT'"
