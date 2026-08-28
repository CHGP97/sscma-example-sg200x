#!/usr/bin/env python3
"""ONVIF device service test for sscma-node.

Exercises the SOAP Device/Media subset and the WS-Discovery responder of a
ReCamera running a stream node with ONVIF enabled. Only the Python standard
library is required.

Usage:
    python3 test_onvif.py <device_ip> [--port 8899] [--username admin] [--password admin]
                          [--profile live]
"""

import argparse
import base64
import hashlib
import os
import socket
import struct
import sys
import time
import urllib.request
import uuid
import xml.etree.ElementTree as ET

SOAP_ENVELOPE = """<?xml version="1.0" encoding="UTF-8"?>
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:tds="http://www.onvif.org/ver10/device/wsdl" xmlns:trt="http://www.onvif.org/ver10/media/wsdl">
<s:Body>{security}{body}</s:Body></s:Envelope>"""

SECURITY_HEADER = """
<s:Header><Security xmlns="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd">
<UsernameToken><Username>{username}</Username>
<Password Type="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest">{digest}</Password>
<Nonce EncodingType="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary">{nonce}</Nonce>
<Created xmlns="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd">{created}</Created>
</UsernameToken></Security></s:Header>"""


def utc_now():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def security_header(username, password):
    nonce = os.urandom(16)
    created = utc_now()
    digest = base64.b64encode(
        hashlib.sha1(nonce + created.encode() + password.encode()).digest()
    ).decode()
    return SECURITY_HEADER.format(
        username=username,
        digest=digest,
        nonce=base64.b64encode(nonce).decode(),
        created=created,
    )


def soap_request(ip, port, action, body, auth=None):
    security = security_header(**auth) if auth else ""
    payload = SOAP_ENVELOPE.format(security=security, body=body).encode()
    req = urllib.request.Request(
        f"http://{ip}:{port}/onvif/device_service",
        data=payload,
        headers={
            "Content-Type": "application/soap+xml; charset=utf-8",
            "SOAPAction": f"http://www.onvif.org/ver10/{action['service']}/wsdl/{action['op']}",
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=5) as resp:
        return resp.read().decode()


# ----------------------------------------------------------------- requests

def op_device(name):
    return {"service": "device", "op": name}


def op_media(name):
    return {"service": "media", "op": name}


def get_system_date_and_time(ip, port):
    body = "<tds:GetSystemDateAndTime/>"
    return soap_request(ip, port, op_device("GetSystemDateAndTime"), body)


def get_device_information(ip, port, auth):
    body = "<tds:GetDeviceInformation/>"
    return soap_request(ip, port, op_device("GetDeviceInformation"), body, auth)


def get_profiles(ip, port, auth):
    body = "<trt:GetProfiles/>"
    return soap_request(ip, port, op_media("GetProfiles"), body, auth)


def get_stream_uri(ip, port, profile, auth):
    body = f"<trt:GetStreamUri><trt:ProfileToken>{profile}</trt:ProfileToken></trt:GetStreamUri>"
    return soap_request(ip, port, op_media("GetStreamUri"), body, auth)


def wsdd_probe(interface_ip, timeout=3.0):
    probe = f"""<?xml version="1.0" encoding="UTF-8"?>
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:a="http://schemas.xmlsoap.org/ws/2004/08/addressing" xmlns:d="http://schemas.xmlsoap.org/ws/2005/04/discovery">
<s:Header><a:MessageID>urn:uuid:{uuid.uuid4()}</a:MessageID><a:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</a:To><a:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</a:Action></s:Header>
<s:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe></s:Body></s:Envelope>"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    mreq = struct.pack("4sl", socket.inet_aton("239.255.255.250"), socket.INADDR_ANY)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
    sock.sendto(probe.encode(), ("239.255.255.250", 3702))
    try:
        while True:
            data, addr = sock.recvfrom(8192)
            text = data.decode(errors="replace")
            if "ProbeMatches" in text and "onvif" in text.lower():
                return addr[0], text
    except socket.timeout:
        return None, None
    finally:
        sock.close()


# -------------------------------------------------------------------- tests

def check(name, ok, detail=""):
    status = "\033[32mPASS\033[0m" if ok else "\033[31mFAIL\033[0m"
    print(f"[{status}] {name}" + (f"  ({detail})" if detail else ""))
    return ok


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ip", help="device IP address")
    parser.add_argument("--port", type=int, default=8899)
    parser.add_argument("--username", default="admin")
    parser.add_argument("--password", default="admin")
    parser.add_argument("--profile", default="live")
    args = parser.parse_args()

    auth = {"username": args.username, "password": args.password}
    failures = 0

    # 1. unauthenticated calls that must work before login
    try:
        r = get_system_date_and_time(args.ip, args.port)
        ok = "GetSystemDateAndTimeResponse" in r and "Fault" not in r
        failures += not check("GetSystemDateAndTime (no auth)", ok)
    except Exception as e:
        failures += not check("GetSystemDateAndTime (no auth)", False, str(e))

    # 2. authenticated calls
    for name, fn in [
        ("GetDeviceInformation", lambda: get_device_information(args.ip, args.port, auth)),
        ("GetProfiles", lambda: get_profiles(args.ip, args.port, auth)),
        (
            "GetStreamUri",
            lambda: get_stream_uri(args.ip, args.port, args.profile, auth),
        ),
    ]:
        try:
            r = fn()
            ok = f"{name}Response" in r and "Fault" not in r
            detail = ""
            if name == "GetProfiles" and ok:
                detail = f"profiles: {r.count('<trt:Profiles ')}"
            if name == "GetStreamUri" and ok:
                uri = r.split("<tt:Uri>")[1].split("</tt:Uri>")[0]
                detail = uri
            failures += not check(name, ok, detail)
        except Exception as e:
            failures += not check(name, False, str(e))

    # 3. GetDeviceInformation without credentials must be refused
    try:
        r = get_device_information(args.ip, args.port, auth={"username": "nope", "password": "nope"})
        failures += not check("GetDeviceInformation rejects bad credentials", "NotAuthorized" in r)
    except Exception as e:
        failures += not check("GetDeviceInformation rejects bad credentials", False, str(e))

    # 4. WS-Discovery
    try:
        addr, text = wsdd_probe(args.ip)
        ok = addr == args.ip and text is not None and "NetworkVideoTransmitter" in text
        xaddr = ""
        if text and "<d:XAddrs>" in text:
            xaddr = text.split("<d:XAddrs>")[1].split("</d:XAddrs>")[0]
        failures += not check("WS-Discovery Probe", ok, xaddr)
    except Exception as e:
        failures += not check("WS-Discovery Probe", False, str(e))

    print()
    if failures:
        print(f"{failures} test(s) failed")
        sys.exit(1)
    print("all tests passed")


if __name__ == "__main__":
    main()
