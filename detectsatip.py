#!/usr/bin/env python3
""" Simple tool to detect SAT>IP devices as JSON.
"""
import json
import socket
import sys
import xml.etree.ElementTree as ET
import requests
import struct
import fcntl

SSDP_ADDR = "239.255.255.250"
SSDP_PORT = 1900
SSDP_MX = 1
SSDP_ST = "urn:ses-com:device:SatIPServer:1"
SSDP_REQUEST = "\r\n".join(
    [
        "M-SEARCH * HTTP/1.1",
        f"HOST: {SSDP_ADDR}:{SSDP_PORT}",
        'MAN: "ssdp:discover"',
        f"MX: {SSDP_MX}",
        f"ST: {SSDP_ST}",
        "USER-AGENT: vdr-detectsatip",
        "\r\n",
    ]
)


def parse_satip_xml(data, url, host_ip):
    """Parse SAT>IP XML data.

    Args:
        data (str): XML input data..
        url (str): URL of SATIP SatIPServer
        host_ip (str): detected via host IP

    Returns:
        dict: Parsed SAT>IP device name and frontend information.
    """
    result = {"name": "", "frontends": {}, "url": url, "detected_via_host_ip": host_ip}
    if data:
        root = ET.fromstring(data)
        name = root.find(".//*/{urn:schemas-upnp-org:device-1-0}friendlyName")
        result["name"] = name.text
        satipcap = root.find(".//*/{urn:ses-com:satip}X_SATIPCAP")
        if satipcap is None:
            # fallback for non-standard Panasonic
            satipcap = root.find(".//*/{urn-ses-com:satip}X_SATIPCAP")
        caps = {}
        for system in satipcap.text.split(","):
            cap = system.split("-")
            if cap:
                count = int(cap[1])
                if cap[0] in caps:
                    count = count + caps[cap[0]]
                caps[cap[0]] = count
        result["frontends"] = caps
    return result

def get_interfaces():
    """Detect all local IPv4 addresses

    Returns:
        list: Found IP addresses
    """
    interfaces = []
    for iface in socket.if_nameindex():
        name = iface[1]
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            ip = socket.inet_ntoa(fcntl.ioctl(s.fileno(), 0x8915,  # SIOCGIFADDR
                                  struct.pack('256s', name[:15].encode("UTF-8") ) )[20:24])
        except socket.error as e:
            sys.stderr.write("ERROR on %s: %s\n" % (name, e))
            continue

        interfaces.append( (name, ip) )

    return interfaces

def detect_satip_devices():
    """Detect available SAT>IP devices by sending a broadcast message.

    Returns:
        list: Found SAT>IP devices.
    """
    urls = []
    devices = []

    for if_name, if_ip in get_interfaces():
        #print("Checking via %s (%s)" % (if_name, if_ip))
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setblocking(0)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        except BaseException:
            pass
        sock.settimeout(1)
        try:
            sock.bind((if_ip, SSDP_PORT))
            sock.sendto(SSDP_REQUEST.encode("utf-8"), (SSDP_ADDR, SSDP_PORT))
        except socket.error as e:
            sys.stderr.write("ERROR on %s (%s): %s - is VDR running?\n" % (if_name, if_ip, e))
            continue

        try:
            while 1:
                data = sock.recv(1024).decode("utf-8")
                if data:
                    for row in data.split("\r\n"):
                        if "LOCATION:" in row:
                            url = row.replace("LOCATION:", "").strip()
                            if url in urls:
                                continue
                            urls.append(url)
                            info = requests.get(url, timeout=2)
                            devices.append(parse_satip_xml(info.text, url, if_ip))
                else:
                    break
        except BaseException:
            pass
        sock.close()
    return devices


if __name__ == "__main__":
    json.dump(detect_satip_devices(), fp=sys.stdout, sort_keys=True, indent=2)
