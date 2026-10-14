#!/usr/bin/env python3
"""Audited DHCPv4 server for QEMU's length-prefixed socket Ethernet netdev."""
import ipaddress
import socket
import struct
import sys
import time

CLIENT_MAC = bytes.fromhex("525400abcdef")
SERVER_MAC = bytes.fromhex("525400000001")
SERVER_IP = ipaddress.IPv4Address("192.0.2.1").packed
LEASE_IP = ipaddress.IPv4Address("192.0.2.123").packed
MASK = ipaddress.IPv4Address("255.255.255.0").packed
ROUTER = ipaddress.IPv4Address("192.0.2.1").packed
COOKIE = b"\x63\x82\x53\x63"


def read_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise EOFError("QEMU socket closed")
        data.extend(chunk)
    return bytes(data)


def parse_options(data):
    result = {}
    pos = 0
    while pos < len(data):
        code = data[pos]
        pos += 1
        if code == 255:
            break
        if code == 0:
            continue
        if pos >= len(data):
            raise ValueError("truncated DHCP option length")
        size = data[pos]
        pos += 1
        if pos + size > len(data):
            raise ValueError("truncated DHCP option value")
        result.setdefault(code, []).append(data[pos:pos + size])
        pos += size
    return result


def opt(code, value):
    return bytes((code, len(value))) + value


def reply(request, message_type, xid, chaddr):
    bootp = bytearray(236)
    bootp[0:3] = b"\x02\x01\x06"
    bootp[4:8] = xid
    bootp[16:20] = LEASE_IP
    bootp[20:24] = SERVER_IP
    bootp[28:34] = chaddr
    bootp.extend(COOKIE)
    bootp.extend(opt(53, bytes((message_type,))))
    bootp.extend(opt(54, SERVER_IP))
    bootp.extend(opt(1, MASK))
    bootp.extend(opt(3, ROUTER))
    bootp.extend(opt(6, ROUTER))
    bootp.extend(opt(51, struct.pack("!I", 3600)))
    bootp.append(255)
    udp = struct.pack("!HHHH", 67, 68, 8 + len(bootp), 0) + bootp
    ip_header = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 0x1234,
                            0, 64, 17, 0, SERVER_IP, b"\xff" * 4)
    # IPv4 header checksum.
    words = struct.unpack("!10H", ip_header)
    total = sum(words)
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    ip_header = ip_header[:10] + struct.pack("!H", (~total) & 0xffff) + ip_header[12:]
    return b"\xff" * 6 + SERVER_MAC + b"\x08\x00" + ip_header + udp


def main():
    port = int(sys.argv[1])
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
    conn.settimeout(30)
    state = []
    xid = chaddr = None
    attempts = 0
    while len(state) < 2:
        frame_len = struct.unpack("!I", read_exact(conn, 4))[0]
        frame = read_exact(conn, frame_len)
        if len(frame) < 14 + 20 + 8 + 240 or frame[12:14] != b"\x08\x00":
            continue
        ip = frame[14:]
        ihl = (ip[0] & 15) * 4
        if ip[9] != 17:
            continue
        udp = ip[ihl:]
        src_port, dst_port, udp_len, _ = struct.unpack_from("!HHHH", udp)
        if (src_port, dst_port) != (68, 67):
            continue
        packet = udp[8:udp_len]
        if len(packet) < 240 or packet[236:240] != COOKIE or packet[0:3] != b"\x01\x01\x06":
            raise ValueError("invalid BOOTP DHCP request")
        req_xid, req_chaddr = packet[4:8], packet[28:34]
        opts = parse_options(packet[240:])
        types = opts.get(53, [])
        if len(types) != 1 or len(types[0]) != 1:
            raise ValueError("request missing a single DHCP message type")
        kind = types[0][0]
        if kind == 1:
            if state:
                if req_xid != xid or req_chaddr != chaddr:
                    raise ValueError("retransmitted Discover changed xid or client MAC")
            else:
                if req_chaddr != CLIENT_MAC:
                    raise ValueError("unexpected DHCP client MAC " + req_chaddr.hex())
                xid, chaddr = req_xid, req_chaddr
                state.append("DISCOVER")
            attempts += 1
            if attempts > 12:
                raise ValueError("too many Discover retries without Request")
            print("SERVER_GOT_DISCOVER xid=%s attempt=%d" % (xid.hex(), attempts), flush=True)
            answer = reply(packet, 2, xid, chaddr)
        elif kind == 3 and state == ["DISCOVER"]:
            if req_xid != xid or req_chaddr != chaddr:
                raise ValueError("Request xid or client MAC differs from Discover")
            if opts.get(50) != [LEASE_IP] or opts.get(54) != [SERVER_IP]:
                raise ValueError("Request did not select the offered address/server")
            state.append("REQUEST")
            print("SERVER_GOT_REQUEST xid=%s" % xid.hex(), flush=True)
            answer = reply(packet, 5, xid, chaddr)
        else:
            raise ValueError("unexpected DHCP message type %d" % kind)
        conn.sendall(struct.pack("!I", len(answer)) + answer)
        print("SERVER_SENT_%s bytes=%d" % ("OFFER" if kind == 1 else "ACK", len(answer)), flush=True)
    print("SERVER_EXCHANGE DISCOVER -> REQUEST", flush=True)
    conn.close()


if __name__ == "__main__":
    main()
