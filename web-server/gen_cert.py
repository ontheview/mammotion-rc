#!/usr/bin/env python3
"""Generate a self-signed TLS certificate for the Luba Remote web server.

Cross-platform (Windows + Linux + macOS), pure-Python via the ``cryptography``
library — no ``openssl`` binary and no PowerShell, so a single code path covers
every host.  ``cryptography`` is already a PyMammotion dependency, so it is
present in the server venv; run this with the venv's Python:

    <venv>/bin/python gen_cert.py                 # POSIX
    <venv>\\Scripts\\python.exe gen_cert.py        # Windows

The cert's Subject Alternative Names cover ``localhost``, the loopback
addresses, this machine's hostname, and its primary LAN IP, so browsing to the
server by LAN IP from a phone doesn't trip a name-mismatch warning (beyond the
expected self-signed-trust prompt).  Default validity is 10 years — a
self-signed cert has no revocation story, so a long life avoids pointless churn.
For a publicly-trusted cert use ``renew-cert.ps1`` (Windows) or certbot/acme.sh
instead and point the launcher at those files.

``install.py`` calls this automatically; you can also re-run it by hand to
regenerate (e.g. after the LAN IP changes).
"""

from __future__ import annotations

import argparse
import datetime as dt
import ipaddress
import socket
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID


def _primary_lan_ip() -> str | None:
    """Best-effort primary LAN IPv4 (the source IP for outbound traffic).

    Uses a connect() on a UDP socket to a public address — no packet is sent,
    it just makes the OS pick the egress interface — then reads back the local
    address.  Returns None if it can't be determined (offline, odd routing).
    """
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        try:
            s.connect(("8.8.8.8", 80))
            return s.getsockname()[0]
        except OSError:
            return None


def _san_entries(extra_hosts: list[str]) -> list[x509.GeneralName]:
    """Build the SAN list, de-duplicated, classifying each value as IP or DNS."""
    names: list[str] = ["localhost", "127.0.0.1", "::1"]
    try:
        names.append(socket.gethostname())
    except OSError:
        pass
    lan = _primary_lan_ip()
    if lan:
        names.append(lan)
    names.extend(extra_hosts)

    seen: set[str] = set()
    entries: list[x509.GeneralName] = []
    for value in names:
        value = value.strip()
        if not value or value in seen:
            continue
        seen.add(value)
        try:
            entries.append(x509.IPAddress(ipaddress.ip_address(value)))
        except ValueError:
            entries.append(x509.DNSName(value))
    return entries


def generate(cert_path: Path, key_path: Path, extra_hosts: list[str], days: int) -> list[str]:
    """Write a self-signed cert/key pair; return the human-readable SAN list."""
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    san = _san_entries(extra_hosts)

    subject = issuer = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Luba Remote")])
    now = dt.datetime.now(dt.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - dt.timedelta(minutes=5))  # small skew allowance
        .not_valid_after(now + dt.timedelta(days=days))
        .add_extension(x509.SubjectAlternativeName(san), critical=False)
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .sign(key, hashes.SHA256())
    )

    key_path.write_bytes(
        key.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.TraditionalOpenSSL,
            encryption_algorithm=serialization.NoEncryption(),
        )
    )
    cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    # Lock the private key down where the OS supports it (no-op on Windows).
    try:
        key_path.chmod(0o600)
    except OSError:
        pass

    return [str(g.value) for g in san]


def main() -> None:
    here = Path(__file__).parent.resolve()
    ap = argparse.ArgumentParser(description="Generate a self-signed TLS cert for the web server.")
    ap.add_argument("--cert", type=Path, default=here / "cert.pem", help="output certificate path")
    ap.add_argument("--key", type=Path, default=here / "key.pem", help="output private-key path")
    ap.add_argument(
        "--host",
        action="append",
        default=[],
        metavar="NAME_OR_IP",
        help="extra hostname/IP to include in the SAN (repeatable)",
    )
    ap.add_argument("--days", type=int, default=3650, help="validity in days (default 3650 = ~10y)")
    args = ap.parse_args()

    args.cert.parent.mkdir(parents=True, exist_ok=True)
    args.key.parent.mkdir(parents=True, exist_ok=True)
    san = generate(args.cert, args.key, args.host, args.days)
    print(f"Wrote {args.cert}")
    print(f"Wrote {args.key}")
    print("Subject Alternative Names:", ", ".join(san))


if __name__ == "__main__":
    main()
