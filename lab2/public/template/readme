# shttpd: A Simple HTTP Server

## 1. Implementation Strategy

This server is implemented in C using low-level POSIX socket APIs and is compliant with HTTP/1.0 (partial support for HTTP/1.1). It supports the following features:

- Accepts only `GET` requests
- Parses and verifies presence of `Host` header
- Handles `Connection: keep-alive` and `Connection: close`
- Uses `sendfile()` for efficient file transfer
- Returns appropriate responses for:
  - 200 OK (with file)
  - 400 Bad Request (malformed request)
  - 404 Not Found (missing file)
  - 500 Internal Server Error (file read error)
- Defaults to:
  - persistent connection for HTTP/1.1
  - non-persistent for HTTP/1.0

## 2. Testing Procedure

The server was tested using:

- Manual `curl` requests:
  - HTTP/1.0 + `Connection: close`
  - HTTP/1.1 + `Connection: keep-alive`
  - Invalid requests (missing Host)
  - Nonexistent file requests
- Automated script for concurrent request testing
- Large file delivery and `sendfile()` correctness
- Persistent request handling loop with repeated `GET`s

## 3. Known Bugs or Limitations

- No support for HTTP methods other than GET
- No timeout handling for incomplete requests
- Does not support range requests or conditional headers
- epoll-based architecture not implemented (bonus omitted)

## 4. Collaborators

This project was implemented individually. No external code or generative AI tools were used.