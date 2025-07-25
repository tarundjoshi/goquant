# High-Performance Trading System

## Overview

This project implements a high-performance trading system designed to provide low-latency market data processing and order execution. It consists of two primary components:

- **DeriClient**: A WebSocket client that connects to Deribit's API, handling authentication, order management, and real-time market data streaming.
- **Streamer**: A WebSocket server that distributes real-time market data to connected clients, manages subscriptions, and maintains local order books.

## Features

- Real-time order book management with incremental updates
- Low-latency WebSocket communication
- Thread-safe concurrent operations
- Efficient JSON parsing using simdjson
- Robust error handling and structured logging with Quill

## Requirements

- **C++17** compatible compiler (GCC or Clang recommended)
- **Boost** libraries (`Beast`, `ASIO`)
- **OpenSSL** for secure WebSocket connections
- **simdjson** for high-performance JSON parsing
- **moodycamel::ConcurrentQueue** for lock-free concurrent queues
- **Quill** for high-performance logging

### Install Dependencies (Ubuntu/Debian)

```
sudo apt-get update 
sudo apt-get install libboost-all-dev libssl-dev cmake git
```

### Third-party Libraries

Clone and build third-party libraries:

**1) simdjson**

```
git clone https://github.com/simdjson/simdjson.git
cd simdjson && mkdir build && cd build
cmake .. && make && sudo make install
```
**2) moodycamel ConcurrentQueue (header-only)**

```
git clone https://github.com/cameron314/concurrentqueue.git
sudo cp concurrentqueue/*.h /usr/local/include/

```

**3) Quill logging**

```
git clone https://github.com/odygrd/quill.git
cd quill && mkdir build && cd build
cmake .. && make && sudo make install

```


## Building the Project

```
make
```


## Running the Project

```
./a.out
```


## Configuration

Adjust the following parameters in `main.cpp` to configure your setup:

| Parameter     | Description                            | Default Value          |
|---------------|----------------------------------------|------------------------|
| `CLIENT_ID`   | Deribit API client ID                  | `zavRq-oY`|
| `KEY_PATH`    | Path to your Ed25519 private key file  |  |
| `STREAM_ADDR` | WebSocket server listening address     | `127.0.0.1`            |
| `STREAM_PORT` | WebSocket server listening port        | `8080`                 |


## Architecture Overview

### DeriClient

Handles communication with Deribit's API:

- Authentication via Ed25519 signatures.
- Order placement, modification, cancellation.
- Market data subscription (order book updates).
- Position management.

### Streamer (WebSocket Server)

Real-time market data distribution:

- Maintains local order books with incremental updates.
- Manages client subscriptions efficiently.
- Broadcasts market data updates to subscribed clients.

#### Key Components:

| Component            | Responsibility                                                     |
|----------------------|--------------------------------------------------------------------|
| `OrderBookManager`   | Maintains current state of order books                             |
| `SubscriptionManager`| Manages client subscriptions and broadcasts updates                |
| `WebSocketSession`   | Handles individual client connections                              |

## Logging

The project uses Quill for ultra-low latency structured logging. Logs are separated into different files under the `/logs` directory:

- **dericlient.log**: Logs related to Deribit API interactions.
- **server.log**: Logs related to WebSocket server operations and client management.
- **performance.log**: Logs related to latency measurements

## Documentation

This project includes detailed documentation generated using **Doxygen**. The documentation provides insights into the project's architecture, usage, and implementation details.

### Viewing the Documentation

To view the documentation:

1. Navigate to the `docs/html/` directory in the project.
2. Open the `index.html` file in your web browser.


### What’s Included in the Documentation?

The Doxygen-generated documentation includes:
- Class hierarchies and relationships.
- Detailed descriptions of all classes, methods, and their parameters.
- Navigation between related components.
- Search functionality for quick access.














