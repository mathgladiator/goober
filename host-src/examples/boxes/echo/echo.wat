;; echo: responds with the request document the host handed the box.
(module
  (import "goober" "goober_request_size" (func $size (result i32)))
  (import "goober" "goober_request_read" (func $read (param i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 2)
  (data (i32.const 0) "application/json")

  (func $echo (result i32)
    (local $n i32)
    ;; Copy the document to offset 1024 (it fits in 2 pages up to ~127 KiB).
    (local.set $n (call $read (i32.const 1024) (i32.const 130048)))
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 1024) (local.get $n)))
    (i32.const 0))

  (func (export "goober_route_echo_get") (result i32) (call $echo))
  (func (export "goober_route_echo_post") (result i32) (call $echo))
  (func (export "goober_route_echo_rest") (result i32) (call $echo))
)
