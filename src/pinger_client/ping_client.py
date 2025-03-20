import time
import threading
import websocket
import json
import statistics

# WebSocket server URL
server_url = "ws://127.0.0.1:8080"

# Global variables for latency measurement
ping_times = {}
latencies = []
lock = threading.Lock()
ping_counter = 0

# Function to handle incoming WebSocket messages
def on_message(ws, message):
    try:
        data = json.loads(message)
        if data.get("action") == "pong" and "id" in data:
            ping_id = data["id"]
            with lock:
                if ping_id in ping_times:
                    start_time = ping_times[ping_id]
                    latency = (time.time() - start_time) * 1000  # Convert to ms
                    latencies.append(latency)
                    print(f"Ping {ping_id} roundtrip: {latency:.2f} ms")
                    del ping_times[ping_id]
    except json.JSONDecodeError:
        print(f"Received non-JSON message: {message}")

# Function to handle WebSocket errors
def on_error(ws, error):
    print(f"WebSocket error: {error}")

# Function to handle WebSocket close
def on_close(ws, close_status_code, close_msg):
    print("WebSocket closed")

# Function to handle WebSocket open
def on_open(ws):
    print("WebSocket connection established")
    
    # Start sending pings
    def ping_thread():
        global ping_counter
        for i in range(100):  # Send 100 pings
            with lock:
                ping_id = ping_counter
                ping_counter += 1
                ping_times[ping_id] = time.time()
                
            ping_msg = json.dumps({"action": "ping", "id": ping_id})
            ws.send(ping_msg)
            time.sleep(0.1)  # Send a ping every 100ms
            
        # After all pings, print statistics
        time.sleep(1)  # Wait for final responses
        with lock:
            if latencies:
                print("\nWebSocket Propagation Latency Statistics (ms):")
                print(f"  Samples: {len(latencies)}")
                print(f"  Min: {min(latencies):.2f}")
                print(f"  Max: {max(latencies):.2f}")
                print(f"  Average: {statistics.mean(latencies):.2f}")
                print(f"  Median: {statistics.median(latencies):.2f}")
                if len(latencies) >= 20:
                    print(f"  95th percentile: {sorted(latencies)[int(len(latencies)*0.95)]:.2f}")
                    print(f"  99th percentile: {sorted(latencies)[int(len(latencies)*0.99)]:.2f}")
                print(f"  Standard deviation: {statistics.stdev(latencies) if len(latencies) > 1 else 0:.2f}")
            else:
                print("No latency measurements collected")
    
    threading.Thread(target=ping_thread).start()

# Initialize WebSocket client
ws = websocket.WebSocketApp(
    server_url,
    on_message=on_message,
    on_error=on_error,
    on_close=on_close,
    on_open=on_open
)

# Start WebSocket client
ws.run_forever()
