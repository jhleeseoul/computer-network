# Project 1: Socket Programming Practice - README

## (1) Implementation Strategy (with Concurrent Server Design)

This project implements two TCP-based network programs: `s-client` and `s-server`.

- **s-client**: Reads input from stdin, sends a request to s-server, receives the response, and prints it.
- **s-server**: Accepts client connections, parses requests, and returns the original message if the request is well-formed.

The server supports concurrent client connections using a **prefork model**.

### Server (`s-server.c`)

1. **Initialization**:
   - Sets `SIGPIPE` to be ignored to prevent crashes.
   - Creates a TCP socket, binds to a port, and listens for connections.
   - Enables `SO_REUSEADDR` to prevent "Address already in use" errors.

2. **Prefork for Concurrency**:
   - Uses `fork()` to create 5 child processes.
   - Each child blocks on `accept()` and handles connections independently.
   - The parent waits for terminated children with `waitpid()`.

3. **Request Handling (`handle_connection`)**:
   - Reads request header up to 1024 bytes and looks for `\r\n\r\n`.
   - Parses header: checks if first line is `POST message SIMPLE/1.0`, and both `Host:` and `Content-length:` exist.
   - Reads request body according to `Content-length` (may include leftover bytes).
   - Sends back a response with `200 OK` and the same message if valid.
   - Sends `400 Bad Request` otherwise.

### Client (`s-client.c`)

1. Parses arguments (`-p` port, `-s` server-IP).
2. Reads up to 10MB from stdin (ignores excess).
3. Connects to the server via TCP.
4. Sends a properly formatted request:
   ```
   POST message SIMPLE/1.0
   Host: <server>
   Content-length: <length>

   <message body>
   ```
5. Receives the server response:
   - If `200 OK`: extracts `Content-length` and prints only the body to stdout.
   - Otherwise: prints the entire response header.

### Robustness and Error Handling
- Uses read/write loops to ensure full transmission.
- Validates `Content-length` field.
- Handles malformed headers and client disconnections gracefully.

### Concurrency Summary
- Simple and robust **prefork model** using `fork()`.
- Each process blocks on `accept()` and handles requests independently.
- Ensures up to **5 concurrent connections** as required.

---

## (2) How I Tested My Programs

- **Basic End-to-End Test**:
  - Ran `s-server` in one terminal: `./s-server -p 1080`
  - Created input file: `echo "Hello, server!" > input.txt`
  - Ran client: `./s-client -p 1080 -s 127.0.0.1 < input.txt`
  - Verified that the response matches the input exactly.

- **Edge Case Tests**:
  - Empty input file: confirmed that client prints an error and exits.
  - Over 10MB input: verified client truncates input to 10MB.
  - Missing `Host:` or `Content-length:` header (via netcat): server returns `400 Bad Request`.

- **Concurrency Test**:
  - Launched multiple `s-client` instances in parallel using background `&`.
  - Verified that server handled multiple simultaneous requests without blocking.

- **Manual Netcat Test**:
  - Sent malformed headers and verified `400 Bad Request`.
  - Sent `Content-length` mismatch: confirmed the server hangs and does not respond (as expected by spec).

---

## (3) Known Bugs

There are currently **no known bugs**.  
The program passes all functional and edge case tests based on the project specification.
