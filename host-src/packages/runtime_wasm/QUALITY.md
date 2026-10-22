# runtime_wasm

The wasmtime engine and the goober-abi@1 boundary: compiling and checking modules, and
running one route export in a fresh store with fuel, an epoch deadline, and a memory cap.

## Quality bar

- A box can import only the functions in `abi_imports()`, from module `goober`. Every
  other import, WASI included, is refused at compile time with its name in the error.
- Every pointer from a box is bounds-checked against its memory; a bad one gets -1, never
  a host crash and never a trap the box didn't cause.
- Each limit has a test that proves it holds: an infinite loop under a timeout, the same
  loop under fuel, and memory growth past the cap.
- Time and randomness come only from the call's input, so the same request and seed give
  the same bytes out. A test proves it.
- The engine is shared and thread-safe. Each call gets its own store, so nothing leaks
  between requests.
