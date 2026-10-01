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

// A stand-in engine for the session tests: every request is answered with the next step of a
// script and recorded, so a test decides every answer and sees every request.
//
//     php tests/scripted_engine.php <dir>
//
// It listens on 127.0.0.1 at a free port, which it writes to <dir>/port. <dir>/script.json is the
// list of steps: {"status": …, "body": …} answers; {"hang": true} reads the request and never
// answers; {"close": true} reads it and closes the connection without a word. Every request is
// appended to <dir>/sent.jsonl as {"verb", "path", "payload"}. A request the script did not expect
// is answered 599, which the driver cannot mistake for one of the engine's answers.

declare(strict_types=1);

$dir = $argv[1];
$server = stream_socket_server('tcp://127.0.0.1:0', $errno, $error);
if ($server === false) {
    fwrite(STDERR, "cannot listen: $error\n");
    exit(1);
}
file_put_contents("$dir/port.tmp", explode(':', (string) stream_socket_get_name($server, false))[1]);
rename("$dir/port.tmp", "$dir/port");

/** @return array{string, string, string}|null the verb, path and body of one request */
function read_request($client): ?array
{
    $head = '';
    while (!str_contains($head, "\r\n\r\n")) {
        $chunk = fread($client, 8192);
        if ($chunk === false || $chunk === '') {
            return null;
        }
        $head .= $chunk;
    }
    [$head, $body] = explode("\r\n\r\n", $head, 2);
    $lines = explode("\r\n", $head);
    [$verb, $path] = explode(' ', $lines[0]) + ['', ''];
    $length = 0;
    foreach ($lines as $line) {
        if (stripos($line, 'Content-Length:') === 0) {
            $length = (int) trim(substr($line, 15));
        }
    }
    while (strlen($body) < $length) {
        $chunk = fread($client, $length - strlen($body));
        if ($chunk === false || $chunk === '') {
            break;
        }
        $body .= $chunk;
    }
    return [$verb, $path, $body];
}

function respond($client, int $status, string $body): void
{
    fwrite($client, "HTTP/1.1 $status Scripted\r\nContent-Type: application/json\r\n"
        . 'Content-Length: ' . strlen($body) . "\r\nConnection: close\r\n\r\n$body");
}

$hung = [];
while (true) {
    $client = @stream_socket_accept($server, -1);
    if ($client === false) {
        continue;
    }
    $request = read_request($client);
    if ($request === null) {
        fclose($client);
        continue;
    }
    [$verb, $path, $body] = $request;
    $sent = ['verb' => $verb, 'path' => $path, 'payload' => $body === '' ? null : json_decode($body, true)];
    file_put_contents("$dir/sent.jsonl", json_encode($sent) . "\n", FILE_APPEND);
    $script = json_decode((string) file_get_contents("$dir/script.json"), true) ?: [];
    $step = array_shift($script);
    file_put_contents("$dir/script.json", json_encode($script));
    if (!is_array($step)) {
        respond($client, 599, '{"unscripted":true}');
    } elseif (!empty($step['hang'])) {
        // Held open and never answered; the driver's own timeout has to end the wait.
        $hung[] = $client;
        continue;
    } elseif (empty($step['close'])) {
        respond($client, (int) $step['status'], (string) $step['body']);
    }
    fclose($client);
}
