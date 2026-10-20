#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# The ps5log/1 server a PS5 payload talks to: what ps5logd does, and no more.
#
#   tools/ps5log-sink.py [--port 9300] [--out klog/ps5log-<time>.log]
#
# Why this exists: the Lapy owned-root daemon (and every probe built with the same
# logging client) calls ps5log_init_default() at startup and refuses to run when the
# channel is not up (source/owned_root_daemon.c: logging_ready == 0 -> return 2 for
# any non-helper build). The channel is up when a TCP connection to DEV_SERVER:DEV_PORT
# is accepted; the destination comes from a dev.conf on the console
# (/app0/dev.conf, /data/homebrew/dev.conf, ./dev.conf) and defaults to port 9300.
#
# Wire format (native/ps5log/ps5log.h, PROTOCOL.md upstream):
#   HELLO ps5log/1 title=<id> app=<name> boot=0x<hex> tag=<tag>
#   <seq>\t<mono_ns>\t<level>\t<text>          (repeated)
#   BYE seq=<last> reason=<text>
# Anything else is stored as level RAW, so this prints every line verbatim.
#
# It writes nothing back: the client is send-only, so a bare accept loop is a
# complete server. Keep it running for the whole session; a daemon that starts while
# nothing listens exits 2 with no trace of its own.
import argparse
import datetime
import socket
import threading
import time


def serve(connection, peer, out, lock):
    def emit(line):
        stamp = datetime.datetime.now().strftime("%H:%M:%S")
        text = f"{stamp} [{peer[0].rsplit('.', 1)[-1]}:{peer[1]}] {line}"
        with lock:
            print(text, flush=True)
            if out:
                out.write(text + "\n")
                out.flush()

    emit("connected")
    buffer = b""
    try:
        while True:
            chunk = connection.recv(65536)
            if not chunk:
                break
            buffer += chunk
            while b"\n" in buffer:
                line, buffer = buffer.split(b"\n", 1)
                emit(line.decode("utf-8", "replace"))
    except OSError as error:
        emit(f"read error: {error}")
    finally:
        if buffer:
            emit(buffer.decode("utf-8", "replace"))
        emit("disconnected")
        connection.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=9300)
    parser.add_argument("--out", default=None, help="also append to this file")
    args = parser.parse_args()

    out = open(args.out, "a", buffering=1) if args.out else None
    lock = threading.Lock()
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("0.0.0.0", args.port))
    listener.listen(8)
    print(f"ps5log sink on :{args.port}" + (f" (also {args.out})" if out else ""), flush=True)
    while True:
        connection, peer = listener.accept()
        threading.Thread(target=serve, args=(connection, peer, out, lock), daemon=True).start()


if __name__ == "__main__":
    main()
