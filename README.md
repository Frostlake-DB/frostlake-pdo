# frostlake-pdo — PDO driver for Frostlake

`pdo_frostlake` is a PHP extension that adds a `frostlake` driver to PDO, so PHP code talks to a
running Frostlake HTTP server the way it talks to Snowflake through `pdo_snowflake`:

```php
$pdo = new PDO('frostlake:host=localhost;port=18082;database=demo;schema=public');
$st = $pdo->prepare('SELECT id, payload:name FROM events WHERE id > ?');
$st->execute([41]);
foreach ($st as $row) { ... }
```

Its surface follows `pdo_snowflake`, the account's own PDO driver: the same data-source keys, values
as strings, the same metadata names, the same multi-statement attribute, and the same gaps. Code
tested against Frostlake meets what it will meet against Snowflake, including what that driver
does not do.

```
┌─────────────┐      PDO       ┌───────────────────┐   HTTP/JSON    ┌────────────────────┐
│ PHP code    │ ─────────────▶ │ pdo_frostlake.so  │ ─────────────▶ │ DatabaseHttpServer │
│             │                │ (this project)    │  /api/execute  │ (frostlake-db jar) │
└─────────────┘                └───────────────────┘                └────────────────────┘
```

The extension has no dependencies beyond PHP and libc. The HTTP client uses POSIX sockets, and the
JSON codec keeps each NUMBER's exact text instead of passing it through a C double. Both come from
`frostlake-odbc`; the HTTP client adds a DELETE call for releasing sessions.

## Requirements

- **PHP 8.1 to 8.5** with PDO, thread-safe or not. Tested with 8.1.34, 8.2.33, 8.3.33, 8.4.24,
  8.4.25 (ZTS) and 8.5.10 on Linux. Windows is not supported: the HTTP client is POSIX sockets.
- **Frostlake engine 0.1.0 or newer**. Ask a running server which one it is with
  `SELECT CURRENT_VERSION()`. The driver is versioned separately from the engine; it speaks the
  HTTP protocol, so this is a minimum version, not a pin.
- A C compiler and PHP's development headers (`php8.4-dev` on Debian and Ubuntu, included in the
  official `php` Docker images).

## Build and install

With [PIE](https://github.com/php/pie), the PHP Foundation's extension installer, which builds,
installs and enables it in one step:

```bash
pie install frostlake/pdo_frostlake
```

With `phpize`, the usual way to build a PHP extension. The build runs in `src/`, so the files it
generates stay out of the way of the repository's own `Makefile`:

```bash
cd src
phpize
./configure
make
sudo make install                 # copies pdo_frostlake.so into the extension directory
echo 'extension=pdo_frostlake' | sudo tee /etc/php/8.4/mods-available/pdo_frostlake.ini
sudo phpenmod pdo_frostlake       # Debian/Ubuntu; elsewhere add the line to php.ini
```

Or with the Makefile, which needs only `php-config` (or the headers' location):

```bash
make                                              # build/pdo_frostlake.so
make PHP_CONFIG=php-config8.4
make PHP_INCLUDE_DIR=/path/to/include/php/20240924
php -d extension=$PWD/build/pdo_frostlake.so -r 'print_r(PDO::getAvailableDrivers());'
```

Without root, the headers can come straight from the package:
`apt-get download php8.4-dev && dpkg-deb -x php8.4-dev_*.deb phpdev`, then
`make PHP_INCLUDE_DIR=$PWD/phpdev/usr/include/php/20240924`.

## Connecting

```
frostlake:host=<host>;port=<port>;database=<db>;schema=<schema>;warehouse=<wh>;role=<role>
```

| key | default | meaning |
|---|---|---|
| `host` | `localhost` | the Frostlake server |
| `port` | `18082` | its HTTP port |
| `role` | — | `USE ROLE`; a role that does not exist refuses the connection |
| `warehouse` | — | `USE WAREHOUSE` |
| `database` | — | `USE DATABASE` |
| `schema` | — | `USE SCHEMA` |

Names are SQL identifiers: a bare name folds to upper case, and a double-quoted one
(`database="mixedCase"`) keeps its case. As in a Snowflake login, a database, schema or warehouse
that does not exist leaves the session without one rather than failing, and a role that does not
exist refuses the connection. Every other key `pdo_snowflake` reads (`account`, `protocol`,
`authenticator`, …) is ignored, as are the user name and password: the server does not
authenticate. A Snowflake DSN therefore needs only its prefix, host and port changed.

Opening a connection checks `GET /api/health` (for at most 10 seconds) and fails with SQLSTATE
`08001` if the server does not answer. Each statement may take up to 300 seconds; the constructor
option `PDO::ATTR_TIMEOUT` changes that limit.

## Behaviour

**Values.** Every value is a PHP string, or `null`, as with `pdo_snowflake`. The string is the
engine's exact text: `NUMBER(38,0)` keeps all 38 digits, `NUMBER(10,2)` reads `1.00`, BINARY is
upper-case hex, VARIANT, OBJECT and ARRAY are JSON text, and dates, times and timestamps are the
engine's text. A BOOLEAN reads `"1"` or `""`, the text libsnowflakeclient gives it.

**Metadata.** `getColumnMeta()` reports what `pdo_snowflake` reports: `name`, `len`, `precision`,
`scale`, `native_type` in Snowflake's result-metadata naming (`FIXED`, `REAL`, `TEXT`, `BOOLEAN`,
`DATE`, `TIME`, `TIMESTAMP_NTZ`/`_LTZ`/`_TZ`, `VARIANT`, `OBJECT`, `ARRAY`, `BINARY`), and
`flags` (`not_null` for a column the engine declares non-nullable). `len` is the column's size in
bytes: four per character for text (`VARCHAR(9)` → 36), the declared length for binary, and 0 for
fixed-size types.

**Parameters.** `?` and `:name` placeholders both work (not mixed in one statement), bound
through `execute([...])`, `bindValue()` or `bindParam()`. The engine has no server-side binding,
so the driver substitutes each value as a literal of its declared type:

| binding | literal |
|---|---|
| `PDO::PARAM_STR` (the default) | a quoted string, with `'` and `\` escaped |
| `PDO::PARAM_INT` | the number; a string that is not a numeric literal stays a quoted string |
| `PDO::PARAM_BOOL` | `TRUE` / `FALSE` |
| `PDO::PARAM_NULL`, or a `null` value | `NULL` |
| `PDO::PARAM_LOB` (a string or a stream) | `X'…'` hex |

The driver finds the placeholders itself, not PDO. PDO's own parser would read the VARIANT path
in `v:field` as a named parameter. Text inside string literals, quoted identifiers, comments and
`$$…$$` bodies is never a placeholder, and neither are `::` casts or a `:` that follows a value
(`PARSE_JSON(x):k`, `arr[0]:k`). With nothing bound, the statement is sent exactly as written, so
a Snowflake Scripting cursor's own `?` (`OPEN c USING (…)`) reaches the engine. A count mismatch,
an unknown name, or mixed styles fail with `HY093`. `debugDumpParams()` shows the SQL that was
sent.

**Several statements.** A request holds one statement unless it asks for more, as on the account:
`Actual statement count 2 did not match the desired statement count 1.` Ask the way
`pdo_snowflake` does:

```php
$pdo->setAttribute(PDO::FROSTLAKE_STMT_MULTI_STMT_COUNT, 2);   // 0 = any number
$st = $pdo->query('SELECT 1 a; SELECT 2 b');
$first = $st->fetchAll();
$st->nextRowset();
$second = $st->fetchAll();
```

`PDO::FROSTLAKE_STMT_MULTI_STMT_COUNT` has the same number as `PDO::SNOWFLAKE_STMT_MULTI_STMT_COUNT`
(1004). As there, a statement takes the connection's count when it is prepared, `PDO::exec()` never
takes it, and the count must match in both directions. `ALTER SESSION SET MULTI_STATEMENT_COUNT`
sets it for the whole session, `exec()` included.

**Counts.** `exec()` and `rowCount()` answer the affected rows for INSERT, UPDATE, DELETE and MERGE,
and the number of rows otherwise, so a DDL statement counts its one status row. A DML statement's
result grid (`number of rows inserted`, …) can still be fetched.

**Transactions.** `beginTransaction()`, `commit()` and `rollBack()` run `BEGIN`, `COMMIT` and
`ROLLBACK`. `PDO::ATTR_AUTOCOMMIT => false` passed to the constructor opens the session with
autocommit off. As with `pdo_snowflake`, which sends autocommit only when it logs in, changing the
attribute later is recorded but does not change the session.

**Sessions.** The server's session is pinned to the connection, so `USE`, variables, temporary
tables and transactions last as long as the connection. Closing the connection releases the
session and rolls back any transaction it left open. If the session disappears (idle timeout,
server restart), the next statement fails with SQLSTATE `08003` instead of running in a new
session without the connection's context. A persistent connection (`PDO::ATTR_PERSISTENT`) is
reused only while its session is still alive; otherwise PDO opens a new one.

**Errors.** A statement the engine refuses fails with SQLSTATE `HY000` and the engine's message in
`errorInfo[2]`; the engine has no native error code, so `errorInfo[1]` is `null`. An unreachable
server is `08S01` (`08001` while connecting), and a statement that outlasts the timeout is
`HYT00`. Every PDO error mode works.

**Not supported, as with `pdo_snowflake`:** `PDO::quote()` (`IM001`), `lastInsertId()` (answers
`false`), statement attributes, and scrollable cursors (rows come forward only).

## Differences from `pdo_snowflake`

- **Value text.** Both drivers return strings, but libsnowflakeclient formats some types itself.
  On Linux and macOS it renders a NUMBER with a scale and a FLOAT through a C double (`12.340000`,
  `1.`), and pads fractional seconds to the column's scale (`2024-01-01 00:00:00.000000000`). This
  driver returns the engine's exact text (`12.34`, `1.0`, `2024-01-01 00:00:00.000`).
- **`native_type` of the newer types.** A GEOGRAPHY, GEOMETRY or VECTOR column reports `TEXT`,
  which is what libsnowflakeclient reports for any type it does not know.
- **No query id.** The engine does not report query ids, so there is no
  `PDO::FROSTLAKE_ATTR_QUERY_ID` equivalent of `PDO::SNOWFLAKE_ATTR_QUERY_ID`.
- **No authentication, no TLS.** The Frostlake server is plain HTTP and does not check credentials.

## Tests

```bash
make test
```

The suite starts a private server (`-Duser.home` in a temporary directory) from the engine jar in
the local Maven repository: `FROSTLAKE_VERSION` (default `0.1.0`), or any classpath through
`FROSTLAKE_CLASSPATH`. To use a running server instead, set `FROSTLAKE_URL=http://host:port`. The
three session tests read session ids from the server's log, so they skip against a server the
harness did not start. `tests/run.sh <script.php>` runs any other script the same way.

## Layout

| File | Role |
|---|---|
| `src/pdo_frostlake.c` | module entry: registers the driver and its attribute constant |
| `src/frostlake_driver.c` | the connection: DSN, PDO connection methods, errors |
| `src/frostlake_statement.c` | statements: execution, result sets, rows, column metadata |
| `src/frostlake_sql.c` | placeholder scanning and literal rendering |
| `src/frostlake_wire.c` | the HTTP protocol: requests, responses, sessions |
| `src/json.c`, `src/http.c` | JSON codec and HTTP client (from `frostlake-odbc`) |
| `src/config.m4` | the `phpize` build, which PIE runs |
| `composer.json` | the package PIE installs from Packagist |
| `Makefile` | the direct build and `make test` |
| `tests/pdo_frostlake_test.php` | the driver's suite |
| `tests/run.sh` | starts a throwaway server and runs a test script against it |
