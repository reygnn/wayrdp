#!/usr/bin/env python3
# For test/stuck-handshake.sh: an X.224 connection request, the server's
# answer read, and then nothing, as a client that stops in the middle of
# the TLS handshake. Prints "held" once the answer came and keeps the
# connection for SECONDS. With SECONDS 0 it only checks that the server
# answers within 5 s (exit 1 if not).
#
# Usage: x224-hold.py PORT SECONDS PROTOCOLS   (1 TLS, 3 TLS and NLA)
import socket
import struct
import sys
import time

port, seconds, protocols = int(sys.argv[1]), float(sys.argv[2]), int(sys.argv[3])
s = socket.create_connection(("127.0.0.1", port), timeout=5)
neg = struct.pack("<BBHI", 0x01, 0, 8, protocols)  # RDP_NEG_REQ
x224 = bytes([len(neg) + 6, 0xE0, 0, 0, 0, 0, 0]) + neg
s.sendall(struct.pack(">BBH", 3, 0, 4 + len(x224)) + x224)
try:
    if not s.recv(1024):
        sys.exit(1)
except socket.timeout:
    sys.exit(1)
print("held", flush=True)
time.sleep(seconds)
