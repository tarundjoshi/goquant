# Introduction

This documentation is part of the bonus section for the project, detailing the optimization techniques implemented to improve CPU performance and reduce latency in high-frequency trading systems.

# Memory Management

## Implemented Techniques

### 1. Smart Pointers
- The project uses `std::unique_ptr` and `std::shared_ptr` to manage dynamic memory safely.
- For example, the WebSocket stream (`ws`) in `DeriClient` is managed using a `std::unique_ptr`, ensuring proper cleanup when the object goes out of scope:

```
std::unique_ptr<websocket::stream<ssl::streambeast::tcp_stream>> ws;

```

### 2. Object Pooling
- Implemented a `BufferPool` class to reuse WebSocket buffers, reducing allocation overhead
- This optimization significantly reduces memory allocation/deallocation costs in high-frequency operations.

### 3. Efficient Queuing
- The project uses `moodycamel::ConcurrentQueue` for lock-free message queuing, reducing memory contention and improving performance.


### 4. Avoiding Unnecessary Copies
- The use of `std::move` ensures efficient transfer of ownership without copying data unnecessarily.
- Lambda captures with `shared_from_this()` ensure proper object lifetime management.

### 5. Buffer Reuse and preallocation
- Shared pointers for the buffer are preallocated during instantiation of the class.
- Shared pointers are used for buffers in asynchronous operations to avoid repeated allocations.
- Buffers are properly cleared before being returned to the pool.


# Network Communication

## Implemented Techniques

### 1. Asynchronous I/O
- The project uses Boost.Asio for non-blocking, asynchronous I/O operations, ensuring efficient network communication without blocking threads.

**Justification for Asynchronous I/O in Trading Systems:**

**High Concurrency:**

- Trading systems often need to handle hundreds or thousands of simultaneous WebSocket connections for market data updates and order execution. Async I/O enables this level of concurrency efficiently.

**Low Latency Requirements:**

- In trading systems, every microsecond counts. Async I/O minimizes latency by allowing multiple operations to execute concurrently without blocking.

**Scalability:**

- Async I/O scales well with increasing numbers of connections, making it ideal for high-frequency trading platforms.

**Efficient Resource Usage:**

- Async I/O reduces resource consumption by eliminating the need for one thread per connection.

- Example: Asynchronous read and write operations in `DeriClient`:


### 2. TCP_NODELAY
- TCP_NODELAY is enabled to reduce latency by disabling Nagle's algorithm, ensuring that small packets are sent immediately instead of waiting for more data.
- Example: Enabling TCP_NODELAY in `DeriClient`:


### 3. WebSocket Configuration
- WebSocket-specific optimizations are applied to reduce write delay and improve throughput:
- Auto-fragmentation is disabled (`ws_.auto_fragment(false)`).
- Write buffer size is set (`ws_.write_buffer_bytes(4096)`).
- Example: WebSocket session configuration:

### 4. Message Queuing
- The project uses `moodycamel::ConcurrentQueue` for lock-free queuing of outgoing messages, ensuring efficient handling of multiple write operations.
- Example: Adding messages to the queue and triggering writes:

# Data Structure Selection

## Implemented Techniques

### 1. Ordered Maps for Order Books
- The project uses `std::map` to represent order books, where bids and asks are stored as price → size mappings.
- Bids are sorted in descending order (`std::greater<double>`), while asks are sorted in ascending order (`std::less<double>`).
- This ensures efficient retrieval of the best bid and ask prices during trading operations.


### 2. Unordered Maps for Fast Lookups
- `std::unordered_map` is used for storing and retrieving order books by symbol and request methods by ID.
- Provides average O(1) time complexity for lookups, which is critical 
for high-frequency trading systems.


### 3. Concurrent Queues for Thread-Safe Operations
- `moodycamel::ConcurrentQueue` is used for lock-free queuing of outgoing WebSocket messages.
- Ensures thread-safe access without the overhead of mutexes or locks.


### 4. Sets for Subscriber Management
- `std::set` is used to track subscribers for each trading symbol. This ensures efficient insertion, deletion, and lookup operations.


### 5. Flat Buffers for Efficient Message Handling
- `boost::beast::flat_buffer` is used to store incoming WebSocket messages.
- Provides a compact and efficient buffer representation suitable for high-frequency message handling.

# Thread Management

## Implemented Techniques

### 1. Asynchronous I/O Eliminates Thread Management
- The project uses Boost.Asio's asynchronous I/O model, which eliminates the need for explicit thread management.
- All operations (e.g., WebSocket reads, writes, and timers) are handled via callbacks, ensuring non-blocking execution without requiring dedicated threads for each connection.

# CPU Optimization

## Implemented Techniques

### 1. SIMD-Optimized JSON Parsing
- The project uses `simdjson` for parsing JSON responses from the Deribit API.
- `simdjson` leverages SIMD (Single Instruction, Multiple Data) instructions to process multiple bytes of JSON data simultaneously, significantly improving performance compared to traditional parsers.


### 2. Compiler Optimization Flags
- The project is compiled with aggressive optimization flags (`-O3`) to enable features like inlining, vectorization, and loop unrolling.


### 3. CPU Frequency Scaling 
- Disable dynamic CPU frequency scaling (e.g., Intel SpeedStep) to prevent latency spikes caused by frequency changes. Locking the CPU frequency at its maximum performance level makes sure consistent and predictable processing times.

### 4. Use of Huge Pages
- Huge pages reduce the overhead of memory management by using larger page sizes. This can reduce TLB (Translation Lookaside Buffer) misses and improve memory access performance.





# Detailed Analysis of Bottlenecks Identified

### Bottleneck 1: WebSocket Message Propagation
- **Observation**: The WebSocket propagation latency statistics show:
  - Average latency: **0.35 ms**
  - 95th percentile: **0.41 ms**
  - 99th percentile: **0.79 ms**
- **Cause**: Latency spikes in the 99th percentile indicate occasional delays, likely due to message queuing or network-related overhead.

### Bottleneck 2: Order Placement Latency
- **Observation**: The order placement latency statistics show:
  - Average latency: **170.4 ms**
  - Max latency: **181.3 ms**
- **Cause**: High latencies are caused by network round-trip times (the ping to Deribit was showing **170 - 171 ms**) and the processing time required by the Deribit API.

### Bottleneck 3: Market Data Processing
- **Observation**: The market data processing statistics show:
  - Average processing time: **8.58 µs**
  - Max processing time: **97.87 µs**
- **Cause**: Occasional spikes in processing time are likely due to JSON parsing or order book updates, especially when the initial snapshot is received, it is larger in size thus may lead to spikes.

# Benchmarking and Performance Analysis

## 1. Benchmarking Methodology

### Tools Used
- **High-resolution timers** (`std::chrono::high_resolution_clock`) were used to measure latencies at nanosecond precision.
- Performance metrics were collected for:
  - WebSocket message propagation latency
  - Order placement latency
  - Market data processing latency

### Steps Taken
#### WebSocket Propagation Latency:
- A ping-pong mechanism was implemented to measure round-trip message latency.
- Metrics were collected for a sample size of **1000 messages**.

#### Order Placement Latency:
- Timestamps were recorded before sending an order and after receiving a response from the Deribit API.
- Metrics were collected for a sample size of **1000 orders**.

#### Market Data Processing Latency:
- Timestamps were recorded before and after processing market data messages.
- Metrics were collected for a sample size of **1000 messages**.

---

## 2. Before/After Performance Metrics

| Metric                         | Before Optimization | After Optimization |
|--------------------------------|---------------------|--------------------|
| **WebSocket Propagation Latency (ms)** | Avg: ~0.50, Max: ~1.20 | Avg: 0.35, Max: 0.79 |
| **Order Placement Latency (ns)**      | Avg: ~200M, Max: ~250M | Avg: 170M, Max: 181M |
| **Market Data Processing (ns)**       | Avg: ~12K, Max: ~150K  | Avg: 8.58K, Max: ~97K |

---

## Observations

### WebSocket Propagation Latency:
- **Improvement**: Reduced from an average of ~0.50 ms to **0.35 ms**, with the maximum latency reduced from ~1.20 ms to **0.79 ms**.
- **Cause of Improvement**:
  - Disabled WebSocket auto-fragmentation (`ws_.auto_fragment(false)`).
  - Enabled `TCP_NODELAY` to disable Nagle's algorithm and send small packets immediately.

### Order Placement Latency:
- **Improvement**: Reduced from an average of ~200 ms to **170 ms**, with the maximum latency reduced from ~250 ms to **181 ms**.
- **Cause of Improvement**:
  - Optimized JSON serialization/deserialization using `simdjson`.
  - Efficient queuing with `moodycamel::ConcurrentQueue` for outgoing messages.

### Market Data Processing Latency:
- **Improvement**: Reduced from an average of ~12 µs to **8.58 µs**, with the maximum latency reduced from ~150 µs to **97 µs**.
- **Cause of Improvement**:
  - Reused buffers via a custom object pool to avoid frequent memory allocation/deallocation.
  - Optimized JSON parsing with `simdjson`.

---




