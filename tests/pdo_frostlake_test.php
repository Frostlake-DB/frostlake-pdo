<?php
// Copyright 2026 MLorek
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// The driver's suite, run through PDO against a live Frostlake server.
//
//     FROSTLAKE_PORT=18082 php -d extension=build/pdo_frostlake.so tests/pdo_frostlake_test.php
//
// tests/run.sh starts a throwaway server and runs this file against it.

declare(strict_types=1);

$host = getenv('FROSTLAKE_HOST') ?: '127.0.0.1';
$port = (int) (getenv('FROSTLAKE_PORT') ?: 18082);
$dsn = "frostlake:host=$host;port=$port";

$passed = 0;
$skipped = [];
$failures = [];
$current = '';

final class Skipped extends Exception
{
}

function check(bool $condition, string $what): void
{
    global $failures, $current;
    if (!$condition) {
        $failures[] = "$current: $what";
    }
}

function same(mixed $expected, mixed $actual, string $what): void
{
    check($expected === $actual, $what . ': expected ' . var_export($expected, true) . ', got ' . var_export($actual, true));
}

/** Runs $work and answers the PDOException it threw, or null when it threw none. */
function refusal(callable $work): ?PDOException
{
    try {
        $work();
    } catch (PDOException $e) {
        return $e;
    }
    return null;
}

function test(string $name, callable $body): void
{
    global $passed, $skipped, $failures, $current;
    $current = $name;
    if (getenv('FROSTLAKE_TEST_VERBOSE')) {
        fwrite(STDERR, "- $name\n");
    }
    $before = count($failures);
    try {
        $body();
    } catch (Skipped $e) {
        $skipped[] = "$name: " . $e->getMessage();
        return;
    } catch (Throwable $e) {
        $failures[] = "$name: unexpected " . get_class($e) . ': ' . $e->getMessage();
    }
    if (count($failures) === $before) {
        $passed++;
    }
}

function connect(array $options = [], string $extra = ''): PDO
{
    global $dsn;
    return new PDO($dsn . $extra, 'someone', 'secret', $options);
}

function server(string $method, string $path): array
{
    global $host, $port;
    $context = stream_context_create(['http' => ['method' => $method, 'ignore_errors' => true, 'timeout' => 10]]);
    $body = file_get_contents("http://$host:$port$path", false, $context);
    return json_decode($body === false ? 'null' : $body, true) ?? [];
}

/**
 * Connects, and answers the connection with the id of the session the server started for it.
 *
 * The id is the wire's, which no SQL function reports, so it is read from the log of the server
 * tests/run.sh started (FROSTLAKE_ENGINE_LOG); attached to any other server, the test skips.
 */
function connect_with_session(array $options = []): array
{
    $log = getenv('FROSTLAKE_ENGINE_LOG');
    if ($log === false || !is_readable($log)) {
        throw new Skipped('needs FROSTLAKE_ENGINE_LOG to learn session ids');
    }
    clearstatcache();
    $offset = filesize($log);
    $pdo = connect($options);
    $pdo->query('SELECT 1');
    return [$pdo, created_session($log, $offset)];
}

function created_session(string $log, int $offset): ?string
{
    clearstatcache();
    $tail = (string) file_get_contents($log, false, null, $offset);
    return preg_match_all('/Created new session: ([0-9a-f-]+)/', $tail, $found) ? end($found[1]) : null;
}

// ---------------------------------------------------------------- the driver

test('driver is registered with its attribute constant', function () {
    check(in_array('frostlake', PDO::getAvailableDrivers(), true), 'frostlake among the drivers');
    same(1004, PDO::FROSTLAKE_STMT_MULTI_STMT_COUNT, 'the constant keeps pdo_snowflake\'s number');
});

test('client version and driver name', function () {
    $pdo = connect();
    same('0.1.0', $pdo->getAttribute(PDO::ATTR_CLIENT_VERSION), 'client version');
    same('frostlake', $pdo->getAttribute(PDO::ATTR_DRIVER_NAME), 'driver name');
    same(1, $pdo->getAttribute(PDO::ATTR_AUTOCOMMIT), 'autocommit reads as an integer');
    $e = refusal(fn () => $pdo->getAttribute(PDO::ATTR_SERVER_VERSION));
    same('IM001', $e?->getCode(), 'an attribute pdo_snowflake lacks is refused');
});

// ---------------------------------------------------------------- connecting

test('an unreachable server refuses the connection', function () {
    $e = refusal(fn () => new PDO('frostlake:host=127.0.0.1;port=1'));
    check($e !== null, 'connecting to a closed port throws');
    same('08001', $e?->errorInfo[0] ?? null, 'SQLSTATE');
    check(str_contains((string) $e?->getMessage(), 'Cannot reach the Frostlake server at 127.0.0.1:1'), 'message names the server: ' . $e?->getMessage());
});

test('a malformed port is refused', function () {
    $e = refusal(fn () => new PDO('frostlake:host=127.0.0.1;port=http'));
    check(str_contains((string) $e?->getMessage(), 'Invalid port in the data source: http'), 'message: ' . $e?->getMessage());
});

test('PDO::connect opens a plain PDO', function () {
    global $dsn;
    if (!method_exists(PDO::class, 'connect')) {
        throw new Skipped('PDO::connect() arrived in PHP 8.4');
    }
    $pdo = PDO::connect($dsn);
    same(PDO::class, get_class($pdo), 'class');
    same('1', $pdo->query('SELECT 1')->fetchColumn(), 'it runs statements');
});

test('database and schema from the data source', function () {
    $setup = connect();
    $setup->exec('CREATE OR REPLACE DATABASE pdo_dsn_db');
    $setup->exec('CREATE OR REPLACE SCHEMA pdo_dsn_db.inner_schema');
    $setup->exec('CREATE OR REPLACE DATABASE "pdoMixed"');

    $pdo = connect([], ';database=pdo_dsn_db;schema=inner_schema;warehouse=COMPUTE_WH');
    $row = $pdo->query('SELECT CURRENT_DATABASE(), CURRENT_SCHEMA(), CURRENT_WAREHOUSE()')->fetch(PDO::FETCH_NUM);
    same(['PDO_DSN_DB', 'INNER_SCHEMA', 'COMPUTE_WH'], $row, 'a bare name folds to upper case');

    $quoted = connect([], ';database="pdoMixed"');
    same('pdoMixed', $quoted->query('SELECT CURRENT_DATABASE()')->fetchColumn(), 'a quoted name keeps its case');
});

test('an unknown database, schema or warehouse leaves the session without it', function () {
    $plain = connect();
    $default = $plain->query('SELECT CURRENT_DATABASE()')->fetchColumn();
    $pdo = connect([], ';database=no_such_db;schema=no_such_schema;warehouse=no_such_wh');
    same($default, $pdo->query('SELECT CURRENT_DATABASE()')->fetchColumn(), 'the database is left as it was');
    $odd = connect([], ';database=not an identifier');
    same($default, $odd->query('SELECT CURRENT_DATABASE()')->fetchColumn(), 'a name that is no identifier selects nothing');
});

test('an unknown role refuses the connection', function () {
    $e = refusal(fn () => connect([], ';role=no_such_role'));
    check($e !== null, 'the connection is refused');
    same('HY000', $e?->errorInfo[0] ?? null, 'SQLSTATE');
    $e = refusal(fn () => connect([], ';role=bad;name'));
    check($e !== null, 'a role that is no identifier is refused');
});

test('a statement that outlasts the timeout', function () {
    $pdo = connect([PDO::ATTR_TIMEOUT => 1]);
    $e = refusal(fn () => $pdo->query('SELECT SYSTEM$WAIT(3)'));
    same('HYT00', $e?->getCode(), 'SQLSTATE');
    check(str_contains((string) $e?->getMessage(), 'did not answer within 1 second'), 'message: ' . $e?->getMessage());
});

test('keys only pdo_snowflake understands are ignored', function () {
    $pdo = connect([], ';account=acme;protocol=https;authenticator=snowflake;application=tests');
    same('1', $pdo->query('SELECT 1')->fetchColumn(), 'the connection works');
});

// ---------------------------------------------------------------- values

test('every value arrives as the engine\'s exact text', function () {
    $pdo = connect();
    $row = $pdo->query("SELECT 12345678901234567890123456789012345678 big, 1::NUMBER(10,2) dec, -0.5::NUMBER(4,3) neg,
        1.5::FLOAT f, 'zażółć' s, TO_BINARY('ABCD') bin, '2024-02-29'::DATE d, NULL nul")->fetch(PDO::FETCH_ASSOC);
    same('12345678901234567890123456789012345678', $row['BIG'], 'NUMBER(38,0) keeps every digit');
    same('1.00', $row['DEC'], 'NUMBER keeps its scale');
    same('-0.500', $row['NEG'], 'a negative fraction');
    same('1.5', $row['F'], 'FLOAT');
    same('zażółć', $row['S'], 'UTF-8 text');
    same('ABCD', $row['BIN'], 'BINARY as upper-case hex');
    same('2024-02-29', $row['D'], 'DATE');
    same(null, $row['NUL'], 'NULL');
});

test('BOOLEAN reads as libsnowflakeclient spells it', function () {
    $pdo = connect();
    $row = $pdo->query('SELECT TRUE t, FALSE f, NULL::BOOLEAN n')->fetch(PDO::FETCH_NUM);
    same(['1', '', null], $row, 'true, false, null');
});

test('semi-structured values read as JSON text', function () {
    $pdo = connect();
    $row = $pdo->query("SELECT PARSE_JSON('{\"a\":[1,2]}') v, ARRAY_CONSTRUCT(1, 'x') a, OBJECT_CONSTRUCT('k', 1) o")->fetch(PDO::FETCH_NUM);
    same(['a' => [1, 2]], json_decode($row[0], true), 'VARIANT');
    same([1, 'x'], json_decode($row[1], true), 'ARRAY');
    same(['k' => 1], json_decode($row[2], true), 'OBJECT');
});

test('temporal values read as text', function () {
    $pdo = connect();
    $row = $pdo->query("SELECT '2024-01-02 03:04:05.123'::TIMESTAMP_NTZ ntz, '12:34:56'::TIME t")->fetch(PDO::FETCH_NUM);
    check(str_starts_with($row[0], '2024-01-02 03:04:05.123'), 'TIMESTAMP_NTZ: ' . $row[0]);
    check(str_starts_with($row[1], '12:34:56'), 'TIME: ' . $row[1]);
});

test('fetch modes', function () {
    $pdo = connect();
    $object = $pdo->query("SELECT 1 a, 'x' b")->fetch(PDO::FETCH_OBJ);
    same('x', $object->B, 'FETCH_OBJ');
    $both = $pdo->query('SELECT 7 a')->fetch(PDO::FETCH_BOTH);
    same(['A' => '7', 0 => '7'], $both, 'FETCH_BOTH');
    $st = $pdo->query('SELECT 1 a UNION ALL SELECT 2 ORDER BY 1');
    $seen = [];
    foreach ($st as $row) {
        $seen[] = $row['A'];
    }
    same(['1', '2'], $seen, 'iterating a statement');
    $st = $pdo->query("SELECT 5 n, 'five' w");
    $st->bindColumn('W', $word);
    $st->fetch(PDO::FETCH_BOUND);
    same('five', $word, 'FETCH_BOUND');
    $lower = connect([PDO::ATTR_CASE => PDO::CASE_LOWER]);
    same(['n' => '1'], $lower->query('SELECT 1 n')->fetch(PDO::FETCH_ASSOC), 'ATTR_CASE');
});

// ---------------------------------------------------------------- metadata

test('column metadata follows pdo_snowflake', function () {
    $pdo = connect();
    $st = $pdo->query("SELECT 1::NUMBER(10,2) n, 1.5::DOUBLE f, 'abc'::VARCHAR(9) s, TRUE b, TO_BINARY('AB') bin,
        PARSE_JSON('1') v, '2024-01-01'::TIMESTAMP_NTZ ts, CURRENT_DATE d");
    same(8, $st->columnCount(), 'columnCount before any fetch');
    $meta = [];
    for ($i = 0; $i < $st->columnCount(); $i++) {
        $meta[] = $st->getColumnMeta($i);
    }
    same(['N', 'F', 'S', 'B', 'BIN', 'V', 'TS', 'D'], array_column($meta, 'name'), 'names');
    same(['FIXED', 'REAL', 'TEXT', 'BOOLEAN', 'BINARY', 'VARIANT', 'TIMESTAMP_NTZ', 'DATE'],
        array_column($meta, 'native_type'), 'native types');
    same(10, $meta[0]['precision'], 'NUMBER precision');
    same(2, $meta[0]['scale'], 'NUMBER scale');
    same(0, $meta[0]['len'], 'a fixed-size type has no byte length');
    same(36, $meta[2]['len'], 'VARCHAR(9) budgets four bytes a character');
    same(1, $meta[3]['len'], 'BOOLEAN');
    same(16777216, $meta[5]['len'], 'VARIANT');
    check(is_array($meta[0]['flags']), 'flags is a list');
    check(!array_key_exists('pdo_type', $meta[0]), 'no pdo_type, as with pdo_snowflake');
    $e = refusal(fn () => $st->getColumnMeta(8));
    same('07009', $e?->getCode(), 'an index past the last column');
});

// ---------------------------------------------------------------- binding

test('positional parameters from execute()', function () {
    $pdo = connect();
    $st = $pdo->prepare('SELECT ? a, ? b, ? c');
    $st->execute(['it\'s', 'back\\slash', '42']);
    same(['A' => 'it\'s', 'B' => 'back\\slash', 'C' => '42'], $st->fetch(PDO::FETCH_ASSOC), 'quote, backslash, digits');
});

test('each declared type renders as its SQL literal', function () {
    $pdo = connect();
    $st = $pdo->prepare('SELECT ? i, ? b, ? n, ? s, TYPEOF(?::VARIANT) t, ? big');
    $st->bindValue(1, 41, PDO::PARAM_INT);
    $st->bindValue(2, false, PDO::PARAM_BOOL);
    $st->bindValue(3, null, PDO::PARAM_NULL);
    $st->bindValue(4, 17);
    $st->bindValue(5, 5, PDO::PARAM_INT);
    $st->bindValue(6, '123456789012345678901234567890', PDO::PARAM_INT);
    $st->execute();
    $row = $st->fetch(PDO::FETCH_NUM);
    same(['41', '', null, '17'], array_slice($row, 0, 4), 'int, bool, null, string');
    same('INTEGER', $row[4], 'an integer binding is a number, not text');
    same('123456789012345678901234567890', $row[5], 'a numeric string bound as an integer keeps its digits');

    $typed = $pdo->prepare('SELECT TYPEOF(?::VARIANT)');
    $typed->bindValue(1, '7');
    $typed->execute();
    same('VARCHAR', $typed->fetchColumn(), 'a string binding stays text');
});

test('an integer binding never lets SQL through', function () {
    $pdo = connect();
    $st = $pdo->prepare('SELECT ? v');
    $st->bindValue(1, '1 OR 1=1', PDO::PARAM_INT);
    $st->execute();
    same('1 OR 1=1', $st->fetchColumn(), 'the text stays a string');
});

test('binary parameters', function () {
    $pdo = connect();
    $st = $pdo->prepare('SELECT ? v');
    $st->bindValue(1, "\x00\xFFab", PDO::PARAM_LOB);
    $st->execute();
    same('00FF6162', $st->fetchColumn(), 'bytes from a string');

    $stream = fopen('php://memory', 'w+b');
    fwrite($stream, "\x01\x02");
    rewind($stream);
    $st->bindValue(1, $stream, PDO::PARAM_LOB);
    $st->execute();
    same('0102', $st->fetchColumn(), 'bytes from a stream');
});

test('a string may hold any byte', function () {
    $pdo = connect();
    $st = $pdo->prepare('SELECT LENGTH(?) n');
    $st->execute(["a\x00b\n\t\"c"]);
    same('7', $st->fetchColumn(), 'NUL, newline, tab and a double quote reach the engine');
});

test('bindParam reads the variable at execution', function () {
    $pdo = connect();
    $st = $pdo->prepare('SELECT ? v');
    $value = 1;
    $st->bindParam(1, $value, PDO::PARAM_INT);
    $value = 2;
    $st->execute();
    same('2', $st->fetchColumn(), 'first execution');
    $value = 3;
    $st->execute();
    same('3', $st->fetchColumn(), 're-execution');
});

test('named parameters', function () {
    $pdo = connect();
    $st = $pdo->prepare('SELECT :a x, :b y, :a z');
    $st->execute([':a' => 'one', 'b' => 'two']);
    same(['X' => 'one', 'Y' => 'two', 'Z' => 'one'], $st->fetch(PDO::FETCH_ASSOC), 'a name used twice, keys with and without a colon');
});

test('a colon or question mark that is not a placeholder', function () {
    $pdo = connect();
    $st = $pdo->prepare("SELECT v:k::INT path, PARSE_JSON(:j):k::INT from_call, :n::INT typed, '?:x' literal, \"q?\" quoted -- :nope ?
        FROM (SELECT PARSE_JSON('{\"k\": 7}') v, 'text' \"q?\") /* ? :nope */");
    $st->execute([':j' => '{"k": 8}', ':n' => '9']);
    same(['PATH' => '7', 'FROM_CALL' => '8', 'TYPED' => '9', 'LITERAL' => '?:x', 'QUOTED' => 'text'],
        $st->fetch(PDO::FETCH_ASSOC), 'paths, casts, strings, identifiers and comments keep their text');
});

test('a negative number after a minus sign stays arithmetic', function () {
    $pdo = connect();
    $st = $pdo->prepare("SELECT 10 -? AS a, 10 -? AS b, 10 -? AS c
        -- a comment the substitution must not open early
        , 1 AS d");
    $st->bindValue(1, -5, PDO::PARAM_INT);
    $st->bindValue(2, '-2', PDO::PARAM_INT);
    $st->bindValue(3, -0.5, PDO::PARAM_INT);
    $st->execute();
    same(['A' => '15', 'B' => '12', 'C' => '10.5', 'D' => '1'], $st->fetch(PDO::FETCH_ASSOC), 'no -- comment appears');
});

test('$$ inside an identifier opens no body', function () {
    $pdo = connect();
    $st = $pdo->prepare('SELECT 1 AS a$$b, ? AS p');
    $st->execute(['x']);
    same(['A$$B' => '1', 'P' => 'x'], $st->fetch(PDO::FETCH_ASSOC), 'the marker after it is substituted');
});

test('a statement with nothing bound goes as written', function () {
    $pdo = connect();
    $script = "DECLARE
        c CURSOR FOR SELECT ? AS v;
        r INTEGER;
    BEGIN
        OPEN c USING (41);
        FETCH c INTO r;
        RETURN r + 1;
    END;";
    same('42', $pdo->query($script)->fetchColumn(), 'a scripting cursor binds its own marker');
});

test('parameter mismatches are HY093', function () {
    $pdo = connect();
    $st = $pdo->prepare('SELECT ? a, ? b');
    $e = refusal(fn () => $st->execute(['only one']));
    same('HY093', $e?->getCode(), 'too few');
    $e = refusal(fn () => $st->execute(['1', '2', '3']));
    same('HY093', $e?->getCode(), 'too many');

    $named = $pdo->prepare('SELECT :a a');
    $e = refusal(fn () => $named->execute([':b' => 1]));
    same('HY093', $e?->getCode(), 'an unknown name');
    check(str_contains((string) $e?->getMessage(), ':a'), 'the message names the missing parameter: ' . $e?->getMessage());

    $mixed = $pdo->prepare('SELECT :a a, ? b');
    $mixed->bindValue(':a', 1);
    $mixed->bindValue(1, 2);
    $e = refusal(fn () => $mixed->execute());
    same('HY093', $e?->getCode(), 'named and positional mixed');
});

test('debugDumpParams shows the SQL that was sent', function () {
    $pdo = connect();
    $st = $pdo->prepare('SELECT ? v');
    $st->execute(["o'k"]);
    ob_start();
    $st->debugDumpParams();
    $dump = ob_get_clean();
    check(str_contains($dump, "Sent SQL: [15] SELECT 'o''k' v"), 'dump: ' . $dump);
});

// ---------------------------------------------------------------- execution

test('exec() answers the affected rows', function () {
    $pdo = connect();
    same(1, $pdo->exec('CREATE OR REPLACE TABLE pdo_exec (id INT, v STRING)'), 'DDL counts its status row, as pdo_snowflake does');
    same(3, $pdo->exec("INSERT INTO pdo_exec VALUES (1, 'a'), (2, 'b'), (3, 'c')"), 'INSERT');
    same(2, $pdo->exec("UPDATE pdo_exec SET v = 'z' WHERE id > 1"), 'UPDATE');
    same(0, $pdo->exec('DELETE FROM pdo_exec WHERE id > 99'), 'DELETE of nothing');
    same(3, $pdo->exec('SELECT * FROM pdo_exec'), 'a query counts its rows');
});

test('rowCount() and the DML result grid', function () {
    $pdo = connect();
    $pdo->exec('CREATE OR REPLACE TABLE pdo_rows (id INT)');
    $st = $pdo->query('INSERT INTO pdo_rows VALUES (1), (2)');
    same(2, $st->rowCount(), 'INSERT');
    same(['number of rows inserted' => '2'], $st->fetch(PDO::FETCH_ASSOC), 'the grid itself is readable');
    $select = $pdo->query('SELECT id FROM pdo_rows');
    same(2, $select->rowCount(), 'a query answers its row count');
    $merge = $pdo->query('MERGE INTO pdo_rows t USING (SELECT 2 id UNION ALL SELECT 3) s ON t.id = s.id
        WHEN MATCHED THEN UPDATE SET id = s.id WHEN NOT MATCHED THEN INSERT VALUES (s.id)');
    same(2, $merge->rowCount(), 'MERGE sums its actions');
});

test('re-executing and closing the cursor', function () {
    $pdo = connect();
    $st = $pdo->prepare('SELECT ? v');
    $st->execute(['a']);
    same('a', $st->fetchColumn(), 'first');
    check($st->closeCursor(), 'closeCursor');
    same(false, $st->fetch(), 'nothing after closing');
    $st->execute(['b']);
    same('b', $st->fetchColumn(), 'second');
    same(false, $st->fetch(), 'forward only, then done');
});

test('session state carries across statements', function () {
    $pdo = connect();
    $pdo->exec('SET pdo_var = 99');
    $pdo->exec('CREATE OR REPLACE TEMPORARY TABLE pdo_temp (v INT)');
    $pdo->exec('INSERT INTO pdo_temp VALUES ($pdo_var)');
    same('99', $pdo->query('SELECT v FROM pdo_temp')->fetchColumn(), 'variable and temporary table');
});

test('several statements in one request', function () {
    $pdo = connect();
    $e = refusal(fn () => $pdo->query('SELECT 1; SELECT 2'));
    check(str_contains((string) $e?->getMessage(), 'Actual statement count 2 did not match the desired statement count 1.'),
        'refused unless asked for: ' . $e?->getMessage());

    $before = $pdo->prepare('SELECT 1 a; SELECT 2 b');
    $pdo->setAttribute(PDO::FROSTLAKE_STMT_MULTI_STMT_COUNT, 2);
    same(2, $pdo->getAttribute(PDO::FROSTLAKE_STMT_MULTI_STMT_COUNT), 'read back');
    $e = refusal(fn () => $before->execute());
    check($e !== null, 'a statement keeps the count it was prepared under');

    $st = $pdo->query("SELECT 1 a; SELECT 'x' b, 'y' c");
    same([['A' => '1']], $st->fetchAll(PDO::FETCH_ASSOC), 'first result set');
    same(true, $st->nextRowset(), 'a second result set');
    same(2, $st->columnCount(), 'its columns');
    same('C', $st->getColumnMeta(1)['name'], 'its metadata');
    same([['B' => 'x', 'C' => 'y']], $st->fetchAll(PDO::FETCH_ASSOC), 'its rows');
    same(false, $st->nextRowset(), 'no third');

    $e = refusal(fn () => $pdo->query('SELECT 1'));
    check(str_contains((string) $e?->getMessage(), 'Actual statement count 1 did not match the desired statement count 2.'),
        'the count is matched both ways: ' . $e?->getMessage());
    $e = refusal(fn () => $pdo->exec('SELECT 1; SELECT 2'));
    check($e !== null, 'exec() does not take the connection\'s count, as with pdo_snowflake');

    $pdo->setAttribute(PDO::FROSTLAKE_STMT_MULTI_STMT_COUNT, 0);
    same('1', $pdo->query('SELECT 1')->fetchColumn(), '0 allows any number');
    $pdo->exec('ALTER SESSION SET MULTI_STATEMENT_COUNT = 0');
    same(1, $pdo->exec('SELECT 1; SELECT 2'), 'the session setting opens exec() too');
});

test('a statement has no attributes to set', function () {
    $pdo = connect();
    $st = $pdo->prepare('SELECT 1');
    $e = refusal(fn () => $st->setAttribute(PDO::FROSTLAKE_STMT_MULTI_STMT_COUNT, 2));
    same('IM001', $e?->getCode(), 'as with pdo_snowflake');
});

// ---------------------------------------------------------------- transactions

test('beginTransaction, commit and rollBack', function () {
    $pdo = connect();
    $other = connect();
    $pdo->exec('CREATE OR REPLACE TABLE pdo_txn (id INT)');

    check($pdo->beginTransaction(), 'begin');
    check($pdo->inTransaction(), 'inTransaction');
    $pdo->exec('INSERT INTO pdo_txn VALUES (1)');
    same('0', $other->query('SELECT COUNT(*) FROM pdo_txn')->fetchColumn(), 'uncommitted work is invisible elsewhere');
    check($pdo->rollBack(), 'rollBack');
    same('0', $pdo->query('SELECT COUNT(*) FROM pdo_txn')->fetchColumn(), 'rolled back');

    $pdo->beginTransaction();
    $pdo->exec('INSERT INTO pdo_txn VALUES (2)');
    $e = refusal(fn () => $pdo->beginTransaction());
    check(str_contains((string) $e?->getMessage(), 'already an active transaction'), 'nested begin: ' . $e?->getMessage());
    check($pdo->commit(), 'commit');
    check(!$pdo->inTransaction(), 'no longer in a transaction');
    same('1', $other->query('SELECT COUNT(*) FROM pdo_txn')->fetchColumn(), 'committed work is visible');
});

test('autocommit is the mode the connection opened with', function () {
    $setup = connect();
    $setup->exec('CREATE OR REPLACE TABLE pdo_autocommit (id INT)');

    $manual = connect([PDO::ATTR_AUTOCOMMIT => false]);
    same(0, $manual->getAttribute(PDO::ATTR_AUTOCOMMIT), 'read back');
    $manual->exec('INSERT INTO pdo_autocommit VALUES (1)');
    same('0', $setup->query('SELECT COUNT(*) FROM pdo_autocommit')->fetchColumn(), 'DML waits for COMMIT');
    $manual->exec('COMMIT');
    same('1', $setup->query('SELECT COUNT(*) FROM pdo_autocommit')->fetchColumn(), 'visible once committed');

    $auto = connect();
    $auto->setAttribute(PDO::ATTR_AUTOCOMMIT, false);
    same(0, $auto->getAttribute(PDO::ATTR_AUTOCOMMIT), 'the attribute is recorded');
    $auto->exec('INSERT INTO pdo_autocommit VALUES (2)');
    same('2', $setup->query('SELECT COUNT(*) FROM pdo_autocommit')->fetchColumn(),
        'but the session keeps autocommitting, as pdo_snowflake sends the mode only when it logs in');
});

test('closing a connection releases its session and rolls back its transaction', function () {
    $setup = connect();
    $setup->exec('CREATE OR REPLACE TABLE pdo_release (id INT)');
    [$pdo, $session] = connect_with_session();
    $pdo->beginTransaction();
    $pdo->exec('INSERT INTO pdo_release VALUES (1)');
    $pdo = null;
    same(['success' => false], array_intersect_key(server('DELETE', "/api/sessions/$session"), ['success' => 0]),
        'the session is gone');
    same('0', $setup->query('SELECT COUNT(*) FROM pdo_release')->fetchColumn(), 'its open transaction was rolled back');
});

test('a connection whose session has gone away says so', function () {
    [$pdo, $session] = connect_with_session();
    $pdo->exec('SET lost_var = 1');
    server('DELETE', "/api/sessions/$session");
    $e = refusal(fn () => $pdo->query('SELECT $lost_var'));
    same('08003', $e?->errorInfo[0] ?? null, 'refused rather than run in a fresh session');
});

test('a persistent connection is reused while its session lives', function () {
    [$first, $session] = connect_with_session([PDO::ATTR_PERSISTENT => 'pdo-suite']);
    $default = $first->query('SELECT CURRENT_DATABASE()')->fetchColumn();
    $first->exec('CREATE OR REPLACE DATABASE pdo_persistent_db');
    $first->exec('USE DATABASE pdo_persistent_db');
    $first = null;
    $again = connect([PDO::ATTR_PERSISTENT => 'pdo-suite']);
    same('PDO_PERSISTENT_DB', $again->query('SELECT CURRENT_DATABASE()')->fetchColumn(), 'the same session, with its state');

    server('DELETE', "/api/sessions/$session");
    [$fresh, $replacement] = connect_with_session([PDO::ATTR_PERSISTENT => 'pdo-suite']);
    $e = refusal(fn () => $again->query('SELECT 1'));
    same('08003', $e?->errorInfo[0] ?? null, 'an object still holding the lost connection reports the loss');
    $again = null;
    check($replacement !== null && $replacement !== $session, 'a lost session means a new connection');
    same($default, $fresh->query('SELECT CURRENT_DATABASE()')->fetchColumn(), 'which starts afresh');
});

// ---------------------------------------------------------------- errors

test('a refused statement raises the engine\'s message', function () {
    $pdo = connect();
    $e = refusal(fn () => $pdo->query('SELECT * FROM no_such_table'));
    same('HY000', $e?->getCode(), 'code');
    same('HY000', $e?->errorInfo[0] ?? null, 'errorInfo SQLSTATE');
    check($e !== null && array_key_exists(1, $e->errorInfo) && $e->errorInfo[1] === null, 'no native code');
    check(str_contains((string) ($e?->errorInfo[2] ?? ''), 'does not exist or not authorized'), 'errorInfo message');
    check(str_starts_with((string) $e?->getMessage(), 'SQLSTATE[HY000]: General error: SQL compilation error:'), 'message: ' . $e?->getMessage());
});

test('silent and warning error modes', function () {
    $pdo = connect([PDO::ATTR_ERRMODE => PDO::ERRMODE_SILENT]);
    same(false, $pdo->query('SELECT * FROM no_such_table'), 'query answers false');
    same('HY000', $pdo->errorCode(), 'connection errorCode');
    check(str_contains((string) $pdo->errorInfo()[2], 'NO_SUCH_TABLE'), 'connection errorInfo');

    $st = $pdo->prepare('SELECT * FROM no_such_table');
    same(false, $st->execute(), 'execute answers false');
    same('HY000', $st->errorCode(), 'statement errorCode');
    check(str_contains((string) $st->errorInfo()[2], 'NO_SUCH_TABLE'), 'statement errorInfo');
    same(true, $st->execute() === false && $pdo->query('SELECT 1') !== false, 'a later statement is unaffected');

    $warn = connect([PDO::ATTR_ERRMODE => PDO::ERRMODE_WARNING]);
    $warning = null;
    set_error_handler(function (int $level, string $message) use (&$warning) {
        $warning = $message;
        return true;
    });
    $warn->exec('DROP TABLE no_such_table');
    restore_error_handler();
    check(str_contains((string) $warning, 'SQLSTATE[HY000]'), 'a warning: ' . $warning);
});

test('what pdo_snowflake leaves out', function () {
    $pdo = connect();
    same(false, $pdo->lastInsertId(), 'lastInsertId answers false');
    $e = refusal(fn () => $pdo->quote('x'));
    same('IM001', $e?->getCode(), 'quote() is not supported');
});

// ---------------------------------------------------------------- summary

$total = $passed + count(array_unique(array_map(fn ($f) => explode(':', $f, 2)[0], $failures)));
echo "pdo_frostlake: $passed of $total tests passed" . ($skipped === [] ? '' : ', ' . count($skipped) . ' skipped') . "\n";
foreach ($skipped as $skip) {
    echo "  SKIP $skip\n";
}
foreach ($failures as $failure) {
    echo "  FAIL $failure\n";
}
exit($failures === [] ? 0 : 1);
