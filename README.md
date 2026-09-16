# frostlake-odbc — native ODBC driver for Frostlake

A C (ODBC 3.x, ANSI) driver that lets any ODBC application — `isql`, pyodbc,
Excel-shaped tooling, BI clients — talk to a running Frostlake HTTP server. It
speaks the same wire protocol as the JDBC HTTP transport (`POST /api/execute`,
JSON in both directions), so both drivers see identical results, update counts
and error messages.

```
┌─────────────┐   ODBC API    ┌──────────────────────┐   HTTP/JSON    ┌────────────────────┐
│ application │ ────────────▶ │ libfrostlakeodbc.so  │ ─────────────▶ │ DatabaseHttpServer │
│ (isql, ...) │   (unixODBC)  │ (this project)       │  /api/execute  │ (frostlake-db jar) │
└─────────────┘               └──────────────────────┘                └────────────────────┘
```

No JVM in the client process, no TLS (the Frostlake server is plain HTTP by
design), no third-party C dependencies — the HTTP client is POSIX sockets and
the JSON codec is local (`src/json.c`), deliberately hand-rolled so NUMBER
cells keep their exact decimal text instead of passing through a C double.

## Engine version

Requires a Frostlake engine **0.0.7 or newer**. Ask a running server which one it is with
`SELECT CURRENT_VERSION()` — every release answers it, so the check works against any engine.

The driver versions independently of the engine: it speaks the HTTP protocol, not
the jar, so this is a floor rather than a lockstep pin.

## Build & test

Requires gcc/make and unixODBC (`sql.h` + `libodbcinst`; Debian/Ubuntu:
`unixodbc unixodbc-dev`).

```bash
make            # build/libfrostlakeodbc.so
make test       # spins up a throwaway Frostlake server (jar from ~/.m2) and
                # runs 199 assertions through the real driver manager
make install    # register driver + "Frostlake" DSN for the current user
                #   (~/.odbcinst.ini, ~/.odbc.ini — no root, reversible with
                #    install/register.sh --remove)
```

Then:

```bash
isql -v Frostlake                       # DSN from make install (localhost:18082)
```

DSN attributes: `Server`, `Port`, `Database`, `Schema`. `Database` and `Schema`
are treated as SQL identifiers, so a bare name folds to upper case the way the
engine folds it; quote the value (`Database="mixedCase"`) to name an object
whose case matters. Connecting waits at most 10 seconds for the server and each
statement at most 300; `SQL_ATTR_LOGIN_TIMEOUT` changes the first of those.
DSN-less connections work too:

```
Driver=Frostlake ODBC Driver;Server=localhost;Port=18082;Database=demo;Schema=public
```

## What is supported

- **Execution**: `SQLExecDirect`, `SQLPrepare`/`SQLExecute`, multi-statement
  batches with `SQLMoreResults` — once the pack has been asked for. A request
  holds one statement until it is, and an unasked-for pack is refused with
  "Actual statement count N did not match the desired statement count D."
  A statement can ask for itself, the way the account's own ODBC driver takes
  it:

  ```c
  #define SQL_SF_STMT_ATTR_MULTI_STATEMENT_COUNT 16385   /* this driver's value */
  SQLSetStmtAttr(stmt, SQL_SF_STMT_ATTR_MULTI_STATEMENT_COUNT, (SQLPOINTER) 2, 0);
  SQLExecDirect(stmt, (SQLCHAR *) "SELECT 1; SELECT 2", SQL_NTS);
  ```

  `0` allows any number and `-1` — the default — hands the decision back to the
  session, exactly as the account's driver means those values. `SQLGetStmtAttr`
  reads it back. The count travels on the request and changes nothing about the
  session, so other statement handles on the connection are unaffected. The
  attribute's NAME and semantics are the account's; the numeric value above is
  this driver's own, because the account does not publish one.
  `ALTER SESSION SET MULTI_STATEMENT_COUNT = n` still works for a whole session.
  DML update counts are derived from the Snowflake-style
  "number of rows inserted/updated/deleted" result (summed — same rule as the
  JDBC driver).
- **Parameters**: input `?` markers, substituted client-side exactly like the
  JDBC transport (strings escape both quote AND backslash; temporals emit
  ISO text with an explicit `::DATE`/`::TIME`/`::TIMESTAMP_NTZ` cast; binary
  binds as `X'..'` hex). Substitution starts once a parameter is bound: with
  nothing bound the text is sent as written, so a Snowflake Scripting cursor's
  own `?` (`DECLARE c CURSOR FOR … WHERE x > ?` … `OPEN c USING (…)`) reaches
  the engine.
- **Results**: `SQLBindCol` + `SQLFetch`, `SQLGetData` with chunked retrieval
  (01004 truncation semantics), conversions to char/all integer widths/
  double/float/bit/date/time/timestamp structs/raw binary (BINARY cells cross
  the wire as hex and are decoded). NUMBER text is exact at any precision.
  A BOOLEAN column is `SQL_BIT`, so its text is `1`/`0`; a boolean in a column
  the engine declares otherwise reads as `true`/`false`.
- **Metadata**: `SQLDescribeCol`/`SQLColAttribute` with the engine's declared
  types (NUMBER(p,s) → `SQL_DECIMAL`; every integer alias → `SQL_BIGINT`,
  since Snowflake's INT/SMALLINT/… are all NUMBER(38,0)); `SQLTables` /
  `SQLColumns` (INFORMATION_SCHEMA-backed), `SQLGetTypeInfo`,
  `SQLStatistics`/`SQLPrimaryKeys`/`SQLSpecialColumns` (truthfully empty —
  the engine keeps no indexes), a broad `SQLGetInfo` set.
- **Transactions**: `SQL_ATTR_AUTOCOMMIT` (mirrored as
  `ALTER SESSION SET AUTOCOMMIT`, like JDBC) and `SQLEndTran` →
  `COMMIT`/`ROLLBACK`.
- **Sessions**: the server assigns a session id on first contact; the driver
  pins it for the connection, so `USE DATABASE`/`USE SCHEMA` and temporary
  state behave per-connection.

## Limitations (deliberate, v1)

- ANSI entry points only; all text is UTF-8. The driver manager maps
  wide-character applications onto them.
- Cursors are forward-only; result sets are fully materialised (that is what
  the wire protocol delivers).
- Input parameters only — no output/array parameters, no `SQLBulkOperations`.
- One in-flight statement per connection (execution is synchronous).
- Linux/unixODBC is the built and tested platform. The code is plain POSIX,
  so iODBC/macOS should be close; Windows would need a Winsock port.

## Layout

| File | Role |
|---|---|
| `src/fl_odbc.h` | internal model: handles, result sets, cells-as-text |
| `src/json.c` | JSON reader/writer (numbers keep their original lexeme) |
| `src/http.c` | POSIX-socket HTTP/1.1 client (Content-Length + chunked) |
| `src/proto.c` | the wire protocol: build SqlRequest, decode SqlResponse |
| `src/handles.c` | SQLAllocHandle/SQLFreeHandle/diagnostics/env attrs |
| `src/connect.c` | SQLConnect/SQLDriverConnect/SQLGetInfo/attrs/SQLEndTran |
| `src/execute.c` | ExecDirect/Prepare/Execute/params/RowCount/MoreResults |
| `src/results.c` | DescribeCol/ColAttribute/BindCol/Fetch/GetData conversions |
| `src/catalog.c` | SQLTables/SQLColumns/SQLGetTypeInfo + empty index answers |
| `test/smoke.c` | 199 assertions through the driver manager |
| `test/smoke.sh` | private registration + throwaway server + smoke run |
| `install/register.sh` | per-user driver/DSN registration (add/remove) |

## A note on `-Bsymbolic`

The driver is linked with `-Wl,-Bsymbolic`, and that flag is load-bearing:
unixODBC's driver manager exports the very same `SQL*` symbol names this
driver does, and the DM sits earlier in the process's global symbol scope.
Without the flag, an intra-driver call (for example the legacy `SQLAllocStmt`
delegating to `SQLAllocHandle`) is routed through the PLT, interposed by the
DM's copy, and the DM then rejects a handle it never issued. With it, the
driver's internal references always bind to its own definitions.
