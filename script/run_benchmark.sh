#!/bin/bash

# Configuration and Paths
SERVER_BIN="./build/lightkv_kv_server"
PORT=11223

# Ensure binaries exist
if [ ! -f "$SERVER_BIN" ]; then
    echo "Error: Server executable ($SERVER_BIN) not found. Please compile the project first."
    exit 1
fi

echo "=================================================="
echo "Starting LightKV Server on port $PORT..."
$SERVER_BIN $PORT &
SERVER_PID=$!

# Ensure the server is shut down when the script exits
trap "echo 'Stopping Server (PID: $SERVER_PID)...'; kill $SERVER_PID 2>/dev/null" EXIT

# Give the server a moment to initialize
sleep 2

echo "=================================================="
echo "Test 1: Pure GET Throughput & Latency"
echo "Command: ./build/lightkv_load_test_client --warmup-keys 200000 --threads 8 --requests 50000 --value-size 128"
./build/lightkv_load_test_client --warmup-keys 200000 --threads 8 --requests 50000 --value-size 128
echo ""

echo "=================================================="
echo "Test 2: Pure SET Throughput & Latency"
echo "Command: ./build/lightkv_rbench_client -t set -n 200000 -c 16 -r 500000 -d 128 --sequential"
./build/lightkv_rbench_client -t set -n 200000 -c 16 -r 500000 -d 128 --sequential
echo ""

echo "=================================================="
echo "Test 3: Mixed Read/Write Throughput"
echo "Command: ./build/lightkv_rbench_client -t get,set,del -n 200000 -c 16 -r 500000 -d 128"
./build/lightkv_rbench_client -t get,set,del -n 200000 -c 16 -r 500000 -d 128
echo ""

echo "=================================================="
echo "Test 4.1: Connection Storm (5K connections)"
echo "Command: ./build/lightkv_conn_stress_test -n 5000 -c 50 --idle"
./build/lightkv_conn_stress_test -n 5000 -c 50 --idle
echo ""

echo "=================================================="
echo "Test 4.2: Connection Storm (10K connections)"
echo "Command: ./build/lightkv_conn_stress_test -n 10000 -c 50 --idle"
./build/lightkv_conn_stress_test -n 10000 -c 50 --idle
echo ""

echo "=================================================="
echo "All tests completed successfully."
