#!/bin/bash

set -euo pipefail

SERVER_BIN="./build/lightkv_kv_server"
TEST_BIN="./build/lightkv_simple_test_client"
PORT="8990"

if [[ ! -x "$SERVER_BIN" ]]; then
  echo "Error: $SERVER_BIN not found. Build the project first."
  exit 1
fi

if [[ ! -x "$TEST_BIN" ]]; then
  echo "Error: $TEST_BIN not found. Build the project first."
  exit 1
fi

echo "Starting LightKV server on port $PORT..."
$SERVER_BIN "$PORT" &
SERVER_PID=$!

trap 'kill "$SERVER_PID" 2>/dev/null || true' EXIT

sleep 2

echo
echo "Smoke test"
$TEST_BIN --mode smoke --host 127.0.0.1 --port "$PORT"

echo
echo "Small write test"
$TEST_BIN --mode set --keys 2000 --requests 2000 --threads 4 --value-size 64 --host 127.0.0.1 --port "$PORT"

echo
echo "Small read test"
$TEST_BIN --mode get --keys 2000 --requests 2000 --threads 4 --host 127.0.0.1 --port "$PORT"

echo
echo "All simple tests completed."