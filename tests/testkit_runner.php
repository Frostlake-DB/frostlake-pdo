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

// Runs the engine-owned, language-neutral JSON test suites through PDO and this driver.
//
//     export FL_CORPUS=/path/to/frostlake/engine/src/test/resources/testkit
//     tests/run.sh tests/testkit_runner.php [--suite <word>]
//
// Suites are read from $FL_CORPUS/suites; without FL_CORPUS the run is skipped. The contract is
// the engine's SCHEMA.md beside them: a reset before each test, steps on one session, values
// compared after normalisation, and a check the transport cannot express (an error code) recorded
// as a missing API rather than failed. tests/pdo_frostlake_test.php runs this too when FL_CORPUS
// is set.

declare(strict_types=1);

const RESET = [
    'ALTER SESSION SET MULTI_STATEMENT_COUNT = 0',
    'CREATE OR REPLACE DATABASE test_db',
    'USE DATABASE test_db',
    'CREATE OR REPLACE SCHEMA test_schema',
    'USE SCHEMA test_schema',
];

// A skip clause names backends; this driver speaks HTTP, so an `http` skip covers it too.
const ALIASES = ['pdo', 'http'];

final class Outcome
{
    public function __construct(
        public array $columns = [],
        public ?array $rows = null,
        public int $updateCount = -1,
        public ?string $error = null,
    ) {
    }
}

function norm(mixed $raw): string
{
    if ($raw === null) {
        return 'NULL';
    }
    $value = trim(is_bool($raw) ? ($raw ? 'true' : 'false') : (string) $raw);
    $lower = strtolower($value);
    if ($value === '' || $lower === 'null') {
        return 'NULL';
    }
    if ($lower === 'true' || $lower === 'false') {
        return strtoupper($lower);
    }
    if (preg_match('/^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$/', $value)) {
        return round_significant($value);
    }
    return $value;
}

/** A decimal rounded to 10 significant digits, as text with no exponent or trailing zeros. */
function round_significant(string $value): string
{
    $negative = $value[0] === '-';
    $value = ltrim($value, '+-');
    $exponent = 0;
    if (preg_match('/^(.*)[eE]([+-]?\d+)$/', $value, $m)) {
        $value = $m[1];
        $exponent = (int) $m[2];
    }
    [$whole, $fraction] = array_pad(explode('.', $value, 2), 2, '');
    $digits = ltrim($whole . $fraction, '0');
    if ($digits === '') {
        return '0';
    }
    // The position of the decimal point relative to the first significant digit.
    $point = strlen($whole) + $exponent - (strlen($whole . $fraction) - strlen($digits));
    if (strlen($digits) > 10) {
        $kept = substr($digits, 0, 10);
        if ($digits[10] >= '5') {
            $kept = bcadd_digits($kept);
            if (strlen($kept) > 10) {
                $kept = substr($kept, 0, 10);
                $point++;
            }
        }
        $digits = $kept;
    }
    $digits = rtrim($digits, '0');
    if ($point <= 0) {
        $text = '0.' . str_repeat('0', -$point) . $digits;
    } elseif ($point >= strlen($digits)) {
        $text = $digits . str_repeat('0', $point - strlen($digits));
    } else {
        $text = substr($digits, 0, $point) . '.' . substr($digits, $point);
    }
    return ($negative ? '-' : '') . $text;
}

/** Adds one to a string of decimal digits. */
function bcadd_digits(string $digits): string
{
    $i = strlen($digits) - 1;
    while ($i >= 0 && $digits[$i] === '9') {
        $digits[$i] = '0';
        $i--;
    }
    return $i < 0 ? '1' . $digits : substr_replace($digits, (string) ((int) $digits[$i] + 1), $i, 1);
}

final class PdoBackend
{
    private PDO $pdo;

    public function __construct(string $dsn)
    {
        $this->pdo = new PDO($dsn, null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
    }

    public function execute(string $sql): Outcome
    {
        try {
            $statement = $this->pdo->query($sql);
        } catch (PDOException $refusal) {
            return new Outcome(error: (string) ($refusal->errorInfo[2] ?? $refusal->getMessage()));
        }
        $columns = [];
        $types = [];
        for ($i = 0; $i < $statement->columnCount(); $i++) {
            $meta = $statement->getColumnMeta($i);
            $columns[] = $meta['name'];
            $types[] = $meta['native_type'];
        }
        $rows = [];
        foreach ($statement->fetchAll(PDO::FETCH_NUM) as $row) {
            $cells = [];
            foreach ($row as $index => $value) {
                $cells[] = cell_text($value, $types[$index] ?? 'TEXT');
            }
            $rows[] = $cells;
        }
        return new Outcome($columns, $rows, derive_update_count($columns, $rows));
    }
}

/** A value as the reference stringifies it: a BOOLEAN as its word, a semi-structured string as its content. */
function cell_text(?string $value, string $type): ?string
{
    if ($value === null) {
        return null;
    }
    if ($type === 'BOOLEAN') {
        return $value === '1' ? 'true' : 'false';
    }
    if (in_array($type, ['VARIANT', 'OBJECT', 'ARRAY'], true)) {
        $decoded = json_decode($value);
        return is_string($decoded) ? $decoded : $value;
    }
    return $value;
}

function derive_update_count(array $columns, array $rows): int
{
    if ($columns === [] || count($rows) !== 1) {
        return -1;
    }
    foreach ($columns as $name) {
        if (!str_starts_with(strtolower((string) $name), 'number of')) {
            return -1;
        }
    }
    $first = trim((string) ($rows[0][0] ?? ''));
    return ctype_digit($first) ? (int) $first : -1;
}

/** [ok, detail, missing API] for one step. */
function check_step(?array $expect, Outcome $result): array
{
    if (isset($expect['error'])) {
        return check_refusal($expect['error'], $result);
    }
    if ($result->error !== null) {
        return [false, 'unexpected error: ' . $result->error, null];
    }
    if (!$expect) {
        return [true, '', null];
    }
    if (array_key_exists('value', $expect)) {
        $actual = $result->rows[0][0] ?? null;
        if (norm($expect['value']) !== norm($actual)) {
            return [false, sprintf('value [%s] != expected [%s]', var_export($actual, true), json_encode($expect['value'])), null];
        }
    }
    if (array_key_exists('rows', $expect)) {
        $want = array_map(fn ($row) => implode("\x1f", array_map('norm', $row)), $expect['rows']);
        $got = array_map(fn ($row) => implode("\x1f", array_map('norm', $row)), $result->rows ?? []);
        if (empty($expect['ordered'])) {
            sort($want);
            sort($got);
        }
        if ($want !== $got) {
            return [false, 'rows differ: expected ' . json_encode($want) . ' got ' . json_encode($got), null];
        }
    }
    if (array_key_exists('rowCount', $expect) && count($result->rows ?? []) !== $expect['rowCount']) {
        return [false, sprintf('rowCount %d != expected %d', count($result->rows ?? []), $expect['rowCount']), null];
    }
    if (array_key_exists('columns', $expect)) {
        $want = array_map('strval', $expect['columns']);
        if (count($want) !== count($result->columns)) {
            return [false, sprintf('column count %d != expected %d %s', count($result->columns), count($want), json_encode($result->columns)), null];
        }
        foreach ($want as $i => $name) {
            if (strtolower($name) !== strtolower((string) $result->columns[$i])) {
                return [false, sprintf('column[%d] [%s] != expected [%s]', $i, $result->columns[$i], $name), null];
            }
        }
    }
    if (array_key_exists('updateCount', $expect) && $result->updateCount !== $expect['updateCount']) {
        return [false, sprintf('updateCount %d != expected %d', $result->updateCount, $expect['updateCount']), null];
    }
    return [true, '', null];
}

function check_refusal(array $error, Outcome $result): array
{
    if ($result->error === null) {
        return [false, 'expected an error, statement succeeded', null];
    }
    $want = $error['messageContains'] ?? null;
    if ($want !== null && stripos($result->error, (string) $want) === false) {
        return [false, sprintf('error message [%s] does not contain [%s]', $result->error, $want), null];
    }
    if (!isset($error['code']) && !isset($error['sqlState'])) {
        return [true, '', null];
    }
    return [true, '', 'ERROR_CODE: cannot check error code/sqlState (backend reports message only)'];
}

$options = getopt('', ['suite:', 'max-report:']);
$filter = $options['suite'] ?? null;
$maxReport = (int) ($options['max-report'] ?? 25);

$corpus = getenv('FL_CORPUS');
if ($corpus === false || $corpus === '') {
    echo "set FL_CORPUS to frostlake's engine/src/test/resources/testkit to replay the testkit corpus\n";
    exit(0);
}
$suites = "$corpus/suites";
$files = glob($suites . '/*.json') ?: [];
if ($files === []) {
    fwrite(STDERR, "FL_CORPUS=$corpus holds no suites/*.json\n");
    exit(1);
}
sort($files);
$host = getenv('FROSTLAKE_HOST') ?: '127.0.0.1';
$port = (int) (getenv('FROSTLAKE_PORT') ?: 18082);
$backend = new PdoBackend("frostlake:host=$host;port=$port");

$counts = ['PASS' => 0, 'FAIL' => 0, 'ERROR' => 0, 'SKIP' => 0];
$report = [];
$missing = [];
$notable = [];
$started = microtime(true);

foreach ($files as $path) {
    $document = json_decode((string) file_get_contents($path), true);
    $suiteName = $document['suite'] ?? basename($path, '.json');
    if ($filter !== null && !str_contains($suiteName, $filter)) {
        continue;
    }
    foreach ($document['tests'] ?? [] as $test) {
        $testName = $test['name'] ?? '?';
        $hit = array_values(array_intersect($test['skip']['backends'] ?? [], ALIASES));
        if ($hit !== []) {
            $counts['SKIP']++;
            $report[] = [$suiteName, $testName, 'SKIP', '', "skip[{$hit[0]}]: " . ($test['skip']['reason'] ?? ''), 0];
            continue;
        }
        $begin = microtime(true);
        $status = 'PASS';
        $failedStep = '';
        $detail = '';
        try {
            foreach (RESET as $sql) {
                $outcome = $backend->execute($sql);
                if ($outcome->error !== null) {
                    throw new RuntimeException("reset failed [$sql]: {$outcome->error}");
                }
            }
            foreach ($test['steps'] ?? [] as $number => $step) {
                if ($step['sql'] === '' && isset($step['expect']['error'])) {
                    // PDO itself refuses an empty statement string, so no driver can send one.
                    $missing[] = 'EMPTY_STATEMENT: PDO refuses an empty statement before the driver sees it';
                    continue;
                }
                [$ok, $why, $absent] = check_step($step['expect'] ?? null, $backend->execute($step['sql']));
                if ($absent !== null) {
                    $missing[] = $absent;
                }
                if (!$ok) {
                    $status = 'FAIL';
                    $failedStep = $number + 1;
                    $detail = $why . ' | sql: ' . substr(str_replace("\n", ' ', $step['sql']), 0, 200);
                    break;
                }
            }
        } catch (Throwable $broke) {
            $status = 'ERROR';
            $detail = get_class($broke) . ': ' . substr(str_replace("\n", ' ', $broke->getMessage()), 0, 200);
        }
        $counts[$status]++;
        $report[] = [$suiteName, $testName, $status, $failedStep, $detail, (int) ((microtime(true) - $begin) * 1000)];
        if (($status === 'FAIL' || $status === 'ERROR') && count($notable) < $maxReport) {
            $notable[] = sprintf('  %-38s %-42s %s %s', $suiteName, $testName, $status, $detail);
        }
    }
}

$out = dirname(__DIR__) . '/build/testkit';
@mkdir($out, 0777, true);
$tsv = "$out/testkit-pdo.tsv";
$lines = ["suite\ttest\tstatus\tfailedStep\tdetail\tms"];
foreach ($report as $row) {
    $lines[] = implode("\t", array_map(fn ($cell) => str_replace(["\t", "\n"], ' ', (string) $cell), $row));
}
file_put_contents($tsv, implode("\n", $lines) . "\n");
if ($missing !== []) {
    $notes = array_count_values($missing);
    ksort($notes);
    $text = "# Missing APIs for backend `pdo`\n\nChecks the suites ask for that this transport cannot express. "
        . "Not failures: the day the API exists they light up.\n\n";
    foreach ($notes as $note => $times) {
        $text .= "- $note ($times checks)\n";
    }
    file_put_contents("$out/missing-apis-pdo.md", $text);
}

$total = array_sum($counts);
printf("\n== testkit corpus through `pdo` ==\n  suites dir : %s\n  tests      : %d\n", $suites, $total);
printf("  PASS %d  FAIL %d  ERROR %d  SKIP %d   in %.1fs\n", $counts['PASS'], $counts['FAIL'], $counts['ERROR'], $counts['SKIP'], microtime(true) - $started);
printf("  report     : %s\n", $tsv);
if ($missing !== []) {
    printf("  missing-API checks recorded: %d\n", count($missing));
}
if ($notable !== []) {
    echo "  first failures:\n", implode("\n", $notable), "\n";
}
exit($counts['FAIL'] > 0 || $counts['ERROR'] > 0 ? 1 : 0);
