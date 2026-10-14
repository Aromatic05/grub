#!/usr/bin/env python3
"""Minimal, audited DHCPv6 server over QEMU's socket Ethernet backend."""
import ipaddress
import socket
import struct
import sys

CLIENT_MAC = bytes.fromhex("525400abcdef")
SERVER_MAC = bytes.fromhex("525400000001")
SERVER_ID = bytes.fromhex("00030001525400000001")  # DUID-LL, Ethernet (type3/hw1/MAC)
LEASE = ipaddress.IPv6Address("2001:db8:1::123").packed


def read_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise EOFError("QEMU socket closed")
        data.extend(chunk)
    return bytes(data)


def checksum(data):
    if len(data) & 1:
        data += b"\0"
    words = struct.unpack("!%dH" % (len(data) // 2), data)
    total = sum(words)
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    return (~total) & 0xffff


def options(data):
    result = {}
    pos = 0
    while pos < len(data):
        if pos + 4 > len(data):
            raise ValueError("truncated DHCPv6 option header")
        code, size = struct.unpack_from("!HH", data, pos)
        pos += 4
        if pos + size > len(data):
            raise ValueError("truncated DHCPv6 option body")
        result.setdefault(code, []).append(data[pos:pos + size])
        pos += size
    return result


def option(code, value):
    return struct.pack("!HH", code, len(value)) + value


def frame(src_ip, dst_ip, xid, msg_type, client_id, iaid):
    iaaddr = LEASE + struct.pack("!II", 3600, 5400)
    ia_na = struct.pack("!III", iaid, 0, 0) + option(5, iaaddr)
    payload = (bytes((msg_type,)) + xid + option(2, SERVER_ID)
               + option(1, client_id) + option(3, ia_na))
    src = ipaddress.IPv6Address("fe80::1").packed
    pseudo = src + dst_ip + struct.pack("!I3xB", 8 + len(payload), 17)
    udp = struct.pack("!HHHH", 547, 546, 8 + len(payload), 0) + payload
    csum = checksum(pseudo + udp)
    udp = udp[:6] + struct.pack("!H", csum) + udp[8:]
    ip6 = struct.pack("!IHBB", 6 << 28, len(udp), 17, 64) + src + dst_ip
    return CLIENT_MAC + SERVER_MAC + struct.pack("!H", 0x86dd) + ip6 + udp


def main():
    port = int(sys.argv[1])
    import time
    deadline = time.monotonic() + 80
    while True:
        conn = socket.socket()
        conn.settimeout(2)
        try:
            conn.connect(("127.0.0.1", port))
            break
        except OSError:
            conn.close()
            if time.monotonic() >= deadline:
                raise TimeoutError("could not connect to QEMU socket netdev")
            time.sleep(0.1)
    conn.settimeout(20)
    observed = []
    retries = 0
    while len(observed) < 2:
        length = struct.unpack("!I", read_exact(conn, 4))[0]
        packet = read_exact(conn, length)
        if len(packet) < 62 or packet[12:14] != b"\x86\xdd":
            continue
        ip = packet[14:]
        if ip[6] != 17:
            continue
        src_ip, dst_ip = ip[8:24], ip[24:40]
        udp = ip[40:]
        src_port, dst_port, udp_len, _ = struct.unpack_from("!HHHH", udp)
        if (src_port, dst_port) != (546, 547):
            continue
        dhcp = udp[8:udp_len]
        if len(dhcp) < 4:
            continue
        msg, xid = dhcp[0], dhcp[1:4]
        opts = options(dhcp[4:])
        if msg == 1:
            if 1 not in opts or 3 not in opts or len(opts[3][0]) < 12:
                raise ValueError("Solicit missing Client Identifier or IA_NA")
            if not observed:
                client_id = opts[1][0]
                iaid = struct.unpack_from("!I", opts[3][0])[0]
                solicit_xid = xid
                observed.append("SOLICIT")
            elif xid != solicit_xid or opts[1][0] != client_id:
                raise ValueError("retransmitted Solicit has mismatched XID/Client ID")
            retries += 1
            if retries > 12:
                raise ValueError("too many Solicit retries without a valid Request")
            print("SERVER_GOT_SOLICIT xid=%s attempt=%d" % (xid.hex(), retries), flush=True)
            reply = frame(src_ip, src_ip, xid, 2, client_id, iaid)
        elif msg == 3 and observed == ["SOLICIT"]:
            if opts.get(2) != [SERVER_ID]:
                raise ValueError("Request missing selected Advertise Server Identifier")
            if xid != solicit_xid:
                raise ValueError("Request transaction ID differs from Solicit")
            if opts.get(1) != [client_id] or struct.unpack_from("!I", opts[3][0])[0] != iaid:
                raise ValueError("Request Client Identifier/IAID differs from Solicit")
            observed.append("REQUEST")
            print("SERVER_GOT_REQUEST xid=%s" % xid.hex(), flush=True)
            reply = frame(src_ip, src_ip, xid, 7, client_id, iaid)
        else:
            raise ValueError("unexpected DHCPv6 message type %d" % msg)
        conn.sendall(struct.pack("!I", len(reply)) + reply)
        print("SERVER_SENT_%s bytes=%d" % ("ADVERTISE" if msg == 1 else "REPLY", len(reply)), flush=True)
    if observed != ["SOLICIT", "REQUEST"]:
        raise ValueError("unexpected exchange order: " + repr(observed))
    print("SERVER_EXCHANGE " + " -> ".join(observed), flush=True)
    conn.close()


if __name__ == "__main__":
    main()
