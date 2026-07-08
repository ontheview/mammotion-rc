#!/usr/bin/env python3
"""One-shot installer for the Luba Remote web server (Windows + Linux + macOS).

The only prerequisite is a Python 3.13 or 3.14 interpreter already installed
(PyMammotion 0.8.x requires ``>=3.13,<3.15``).  Run this file with that Python:

    python install.py            # Windows — run from an *Administrator* prompt
    python3 install.py           # Linux / macOS

On Windows the installer must be run elevated (Administrator): it adds a Windows
Firewall rule so proxy auto-discovery (broadcast UDP) and LAN access to the web
UI get through.  It exits early with instructions if not elevated.

It is idempotent — safe to re-run to repair or upgrade an install.  Each step
below can be pinned via a CLI flag (see ``--help``); anything not supplied is
prompted for on a terminal, or defaulted in ``--non-interactive`` mode.

What it does:
  1. Locate a compatible Python and create a virtualenv (default: ./.venv).
  2. pip install -r requirements.txt into it (clean upstream PyMammotion 0.8.9 +
     FastAPI/uvicorn; our HC33 transport ships in this repo, nothing is patched).
  3. TLS certificate — either generate a self-signed one (cross-platform, via
     gen_cert.py) or point at an existing publicly-trusted cert/key.
  4. Credentials — web-UI login password (recommended) written to secrets.toml;
     the Mammotion account is optional here and can instead be entered later in
     the web onboarding page.
  5. Emit run-server.bat + run-server.sh with the venv/cert paths baked in, so
     there is no line to hand-edit.
  6. (Windows only) Add a Windows Firewall inbound-allow rule for the venv
     Python so the UDP proxy-discovery replies and LAN access to the UI pass.
"""

from __future__ import annotations

import argparse
import getpass
import os
import subprocess
import sys
import tomllib
from pathlib import Path

APP_DIR = Path(__file__).parent.resolve()
IS_WINDOWS = os.name == "nt"

MIN_PY = (3, 13)
MAX_PY_EXCL = (3, 15)


# ── small utilities ───────────────────────────────────────────────────────────
def info(msg: str) -> None:
    print(f"  {msg}")


def step(msg: str) -> None:
    print(f"\n\033[1m==>\033[0m {msg}" if not IS_WINDOWS else f"\n==> {msg}")


def die(msg: str) -> None:
    print(f"\nERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    info("$ " + " ".join(str(c) for c in cmd))
    return subprocess.run(cmd, check=True, **kw)


def toml_quote(s: str) -> str:
    """Quote a value as a TOML basic string (matches persist._toml_quote)."""
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def interactive(args: argparse.Namespace) -> bool:
    return not args.non_interactive and sys.stdin.isatty()


def _is_admin() -> bool:
    """True if this process is elevated (Administrator). Windows only."""
    try:
        import ctypes
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def require_admin() -> None:
    """On Windows, refuse to run unless elevated. We add a Windows Firewall rule
    (step 6) so proxy-discovery UDP replies and LAN access to the web UI get
    through Defender Firewall, and that needs Administrator rights. No-op on
    Linux/macOS (never require root there)."""
    if not IS_WINDOWS or _is_admin():
        return
    die(
        "This installer must be run as Administrator on Windows.\n"
        "  It adds a Windows Firewall rule so proxy auto-discovery (UDP) and LAN\n"
        "  access to the web UI work. Re-run from an elevated prompt:\n"
        "    1. Start menu -> type 'cmd' -> right-click 'Command Prompt'\n"
        "       -> 'Run as administrator'\n"
        f"    2. cd {APP_DIR}\n"
        "    3. python install.py"
    )


# ── 1. interpreter + venv ─────────────────────────────────────────────────────
def _version_of(cmd: list[str]) -> tuple[int, int] | None:
    try:
        out = subprocess.run(
            [*cmd, "-c", "import sys;print(sys.version_info[0],sys.version_info[1])"],
            capture_output=True, text=True, timeout=15,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if out.returncode != 0:
        return None
    try:
        major, minor = (int(x) for x in out.stdout.split())
        return (major, minor)
    except ValueError:
        return None


def _compatible(ver: tuple[int, int] | None) -> bool:
    return ver is not None and MIN_PY <= ver < MAX_PY_EXCL


def find_interpreter(explicit: str | None) -> list[str]:
    """Return a command (as a list) for a Python in [3.13, 3.15)."""
    if explicit:
        cmd = [explicit]
        if not _compatible(_version_of(cmd)):
            die(f"{explicit} is not a Python {MIN_PY[0]}.{MIN_PY[1]}–3.14 interpreter.")
        return cmd

    # Prefer the interpreter running this script if it qualifies.
    if _compatible(sys.version_info[:2]):
        return [sys.executable]

    candidates: list[list[str]] = []
    if IS_WINDOWS:
        candidates += [["py", "-3.14"], ["py", "-3.13"]]
    candidates += [["python3.14"], ["python3.13"], ["python3"], ["python"]]
    for cmd in candidates:
        if _compatible(_version_of(cmd)):
            return cmd

    die(
        "No Python 3.13 or 3.14 found. Install one "
        + ("(https://www.python.org/downloads/) " if IS_WINDOWS else "(e.g. deadsnakes / your package manager) ")
        + "and re-run, or pass --python PATH."
    )
    raise AssertionError  # unreachable


def venv_python(venv_dir: Path) -> Path:
    return venv_dir / ("Scripts/python.exe" if IS_WINDOWS else "bin/python")


def setup_venv(venv_dir: Path, interp: list[str], recreate: bool) -> Path:
    vpy = venv_python(venv_dir)
    if venv_dir.exists() and not recreate:
        if vpy.exists():
            info(f"Reusing existing venv at {venv_dir}")
        else:
            die(f"{venv_dir} exists but has no interpreter — pass --recreate to rebuild it.")
    else:
        if venv_dir.exists():
            import shutil
            info(f"Removing existing venv at {venv_dir}")
            shutil.rmtree(venv_dir)
        run([*interp, "-m", "venv", str(venv_dir)])
    return vpy


def pip_install(vpy: Path) -> None:
    run([str(vpy), "-m", "pip", "install", "--upgrade", "pip"])
    run([str(vpy), "-m", "pip", "install", "-r", str(APP_DIR / "requirements.txt")])
    # Fail loudly here rather than at first server start.
    run([str(vpy), "-c", "import fastapi, uvicorn, pymammotion, cryptography; print('deps OK')"])


# ── 3. certificate ────────────────────────────────────────────────────────────
def setup_cert(args: argparse.Namespace, vpy: Path) -> tuple[Path, Path]:
    mode = args.cert_mode
    if mode is None:
        if args.cert and args.key:
            mode = "existing"
        elif interactive(args):
            ans = input("\nTLS cert — [S]elf-signed (default) or point at an [E]xisting one? [S/e] ").strip().lower()
            mode = "existing" if ans.startswith("e") else "self-signed"
        else:
            mode = "self-signed"

    if mode == "existing":
        cert = Path(args.cert) if args.cert else None
        key = Path(args.key) if args.key else None
        if interactive(args):
            if not cert:
                cert = Path(input("  Path to certificate (PEM): ").strip())
            if not key:
                key = Path(input("  Path to private key (PEM): ").strip())
        if not cert or not key:
            die("--cert and --key are required for an existing certificate.")
        cert, key = cert.expanduser().resolve(), key.expanduser().resolve()
        if not cert.is_file() or not key.is_file():
            die(f"cert/key not found: {cert} / {key}")
        info(f"Using existing certificate: {cert}")
        return cert, key

    # self-signed
    cert = (Path(args.cert) if args.cert else APP_DIR / "cert.pem").resolve()
    key = (Path(args.key) if args.key else APP_DIR / "key.pem").resolve()
    if cert.exists() and key.exists() and not args.force_cert:
        info(f"Reusing existing self-signed cert ({cert.name}); pass --force-cert to regenerate.")
        return cert, key
    cmd = [str(vpy), str(APP_DIR / "gen_cert.py"), "--cert", str(cert), "--key", str(key)]
    for h in args.cert_host:
        cmd += ["--host", h]
    run(cmd)
    return cert, key


# ── 4. credentials (secrets.toml) ─────────────────────────────────────────────
def setup_secrets(args: argparse.Namespace) -> None:
    path = APP_DIR / "secrets.toml"
    existing: dict = {}
    if path.exists():
        with path.open("rb") as f:
            existing = tomllib.load(f)

    email = existing.get("email")
    password = existing.get("password")
    web_username = existing.get("web_username")
    web_password = existing.get("web_password")

    # Web-UI login (the gate on the whole UI + joystick socket).
    if args.web_password is not None:
        web_password = args.web_password or None
    elif interactive(args):
        prompt = "\nWeb-UI login password"
        prompt += " [keep current]: " if web_password else " (blank = UNPROTECTED): "
        entered = getpass.getpass(prompt)
        if entered:
            web_password = entered
    if not web_password:
        info("No web password set — the UI will be UNPROTECTED on your LAN.")

    # Mammotion account (optional here; onboarding can do it later).
    if args.email is not None:
        email = args.email or email
        password = args.password if args.password is not None else password
    elif interactive(args):
        current = f" [keep {email}]" if email else " (blank = set up later in onboarding)"
        entered = input(f"\nMammotion account email{current}: ").strip()
        if entered:
            email = entered
            password = getpass.getpass("  Mammotion account password: ") or password

    lines: list[str] = ["# Written by install.py. Gitignored — never commit.\n"]
    if email:
        lines.append(f"email    = {toml_quote(email)}\n")
    if password:
        lines.append(f"password = {toml_quote(password)}\n")
    if web_username:
        lines.append(f"web_username = {toml_quote(web_username)}\n")
    if web_password:
        lines.append(f"web_password = {toml_quote(web_password)}\n")

    tmp = path.with_suffix(".toml.tmp")
    tmp.write_text("".join(lines), encoding="utf-8")
    try:
        os.chmod(tmp, 0o600)
    except OSError:
        pass
    os.replace(tmp, path)
    info(f"Wrote {path.name}" + (" (Mammotion account pending — do it in onboarding)" if not email else ""))


# ── 5. launchers ──────────────────────────────────────────────────────────────
def write_launchers(vpy: Path, cert: Path, key: Path, host: str, port: int) -> list[Path]:
    written: list[Path] = []

    bat = APP_DIR / "run-server.bat"
    bat.write_text(
        "@echo off\r\n"
        "REM Generated by install.py - do not hand-edit; re-run install.py to change paths.\r\n"
        f'set "VENV_PY={vpy}"\r\n'
        f'set "HERE=%~dp0"\r\n'
        'if not exist "%VENV_PY%" (\r\n'
        "  echo [ERROR] venv Python not found at %VENV_PY%\r\n"
        "  echo Re-run: python install.py\r\n"
        "  pause & exit /b 1\r\n"
        ")\r\n"
        '"%VENV_PY%" -c "import fastapi, uvicorn, pymammotion" || (\r\n'
        "  echo [ERROR] deps missing — re-run: python install.py\r\n"
        "  pause & exit /b 1\r\n"
        ")\r\n"
        'cd /d "%HERE%"\r\n'
        f"echo Starting server on port {port} -- the browsable https URL is printed below  (Ctrl+C to stop)\r\n"
        f'"%VENV_PY%" -m uvicorn app:app --host {host} --port {port} ^\r\n'
        f'  --ssl-keyfile "{key}" --ssl-certfile "{cert}" ^\r\n'
        "  --timeout-graceful-shutdown 2\r\n"
        "pause\r\n",
        encoding="utf-8",
    )
    written.append(bat)

    sh = APP_DIR / "run-server.sh"
    sh.write_text(
        "#!/usr/bin/env bash\n"
        "# Generated by install.py - do not hand-edit; re-run install.py to change paths.\n"
        'set -euo pipefail\n'
        f'VENV_PY="{vpy}"\n'
        f'cd "{APP_DIR}"\n'
        'if [ ! -x "$VENV_PY" ]; then echo "venv Python missing — re-run: python3 install.py" >&2; exit 1; fi\n'
        f'echo "Starting server on port {port} -- the browsable https URL is printed below  (Ctrl+C to stop)"\n'
        f'exec "$VENV_PY" -m uvicorn app:app --host {host} --port {port} \\\n'
        f'  --ssl-keyfile "{key}" --ssl-certfile "{cert}" \\\n'
        "  --timeout-graceful-shutdown 2\n",
        encoding="utf-8",
    )
    try:
        sh.chmod(0o755)
    except OSError:
        pass
    written.append(sh)
    return written


# ── 6. Windows Firewall ───────────────────────────────────────────────────────
FIREWALL_RULE_NAME = "Mammotion Remote web-server"


def setup_firewall(vpy: Path) -> None:
    """Add a Windows Firewall inbound-allow rule for the venv Python.

    Proxy discovery broadcasts a UDP probe and the HC33s reply *unicast* from
    their own IPs to an ephemeral source port; Defender Firewall's stateful UDP
    pinhole is keyed on the broadcast destination, so those replies look
    unsolicited and are dropped without an inbound rule. The rule is scoped by
    PROGRAM (not port) so it covers both the ephemeral UDP replies and inbound
    LAN connections to the web UI on 8443. Requires Administrator — guaranteed
    by require_admin() at startup. Idempotent: it replaces any prior rule of the
    same name. No-op off Windows."""
    if not IS_WINDOWS:
        return
    # Drop a stale rule first so re-runs don't stack duplicates (rc!=0 when none
    # match — ignore it).
    subprocess.run(
        ["netsh", "advfirewall", "firewall", "delete", "rule", f"name={FIREWALL_RULE_NAME}"],
        capture_output=True,
    )
    add = [
        "netsh", "advfirewall", "firewall", "add", "rule",
        f"name={FIREWALL_RULE_NAME}",
        "dir=in", "action=allow",
        f"program={vpy}",
        "enable=yes", "profile=private,domain",
    ]
    info("$ " + " ".join(add))
    res = subprocess.run(add, capture_output=True, text=True)
    if res.returncode != 0:
        die(
            "Failed to add the Windows Firewall rule (elevation lost?):\n  "
            + (res.stderr or res.stdout or "").strip()
        )
    info(f"Inbound-allow rule '{FIREWALL_RULE_NAME}' added for {vpy.name} "
         "(profiles: private, domain).")


# ── main ──────────────────────────────────────────────────────────────────────
def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--python", help="Interpreter to build the venv from (default: auto-detect 3.13/3.14)")
    ap.add_argument("--venv", type=Path, default=APP_DIR / ".venv", help="venv location (default: ./.venv)")
    ap.add_argument("--recreate", action="store_true", help="Delete and rebuild the venv")
    ap.add_argument("--cert-mode", choices=["self-signed", "existing"], help="Certificate strategy")
    ap.add_argument("--cert", help="Certificate PEM path (output for self-signed, input for existing)")
    ap.add_argument("--key", help="Private-key PEM path")
    ap.add_argument("--cert-host", action="append", default=[], metavar="NAME_OR_IP",
                    help="Extra SAN host/IP for the self-signed cert (repeatable)")
    ap.add_argument("--force-cert", action="store_true", help="Regenerate the self-signed cert even if one exists")
    ap.add_argument("--web-password", help="Web-UI login password ('' = unprotected)")
    ap.add_argument("--email", help="Mammotion account email")
    ap.add_argument("--password", help="Mammotion account password")
    ap.add_argument("--host", default="0.0.0.0", help="Bind address (default 0.0.0.0)")
    ap.add_argument("--port", type=int, default=8443, help="Bind port (default 8443)")
    ap.add_argument("--non-interactive", action="store_true", help="Never prompt; use flags/defaults/existing values")
    args = ap.parse_args()

    require_admin()  # Windows: must be elevated to add the firewall rule (step 6)

    venv_dir = args.venv.resolve()

    step("Locating a compatible Python (3.13 / 3.14)")
    interp = find_interpreter(args.python)
    info(f"Using: {' '.join(interp)}")

    step(f"Creating virtualenv at {venv_dir}")
    vpy = setup_venv(venv_dir, interp, args.recreate)

    step("Installing dependencies (this pulls PyMammotion 0.8.9 and its stack)")
    pip_install(vpy)

    step("Setting up the TLS certificate")
    cert, key = setup_cert(args, vpy)

    step("Configuring credentials (secrets.toml)")
    setup_secrets(args)

    step("Writing launchers")
    launchers = write_launchers(vpy, cert, key, args.host, args.port)

    if IS_WINDOWS:
        step("Adding Windows Firewall rule (inbound allow: proxy discovery + LAN UI)")
        setup_firewall(vpy)

    run_cmd = "run-server.bat" if IS_WINDOWS else "./run-server.sh"
    print("\n" + "=" * 60)
    print("Install complete.")
    for p in launchers:
        info(f"launcher: {p}")
    print("\nStart the server with:")
    print(f"    {run_cmd}")
    try:
        from gen_cert import _primary_lan_ip
        _ip = _primary_lan_ip() or "<this-machine-ip>"
    except Exception:
        _ip = "<this-machine-ip>"
    print(f"\nThen browse to  https://{_ip}:{args.port}/  from any device on the LAN.")
    print("(The 0.0.0.0 address Uvicorn prints is the bind address, not a URL to open.)")
    print("First run with no mowers configured lands on the onboarding page.")
    print("=" * 60)


if __name__ == "__main__":
    main()
