#!/usr/bin/env python3
# For test/mwc-rdp.sh: an X.224 connection request asking for TLS and NLA,
# then a TLS handshake the client aborts because it doesn't trust the
# certificate, as mstsc does before its certificate prompt.
#
# Usage: tls-abort.py PORT
import socket
import ssl
import struct
import sys

port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port))
neg = struct.pack("<BBHI", 0x01, 0, 8, 0x3)  # RDP_NEG_REQ: SSL | HYBRID
x224 = bytes([len(neg) + 6, 0xE0, 0, 0, 0, 0, 0]) + neg
s.sendall(struct.pack(">BBH", 3, 0, 4 + len(x224)) + x224)
s.recv(1024)
ctx = ssl.create_default_context()  # verifies the certificate: fails
try:
    ctx.wrap_socket(s, server_hostname="wayrdp.invalid")
except ssl.SSLError as e:
    print("client aborted:", e.reason)
