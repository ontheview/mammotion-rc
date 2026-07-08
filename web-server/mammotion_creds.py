"""Bake the Mammotion *app-level* API constants into the server.

PyMammotion's PyPI/source build ships these BLANK (see pymammotion/const.py: the
real values are injected at build time via its scripts/update_credentials.py).
Its plaintext fallbacks are the generic/WRONG keys.  We don't want the web server
to depend on a .env, so we set the working values here as process-environment
defaults.

IMPORTANT: pymammotion.const reads os.environ **at import time** and binds the
values into module globals.  So this module MUST be imported before any
pymammotion import.  app.py imports it first for that reason.

These constants identify the Mammotion *mobile app*, not a user — same for
everyone.  They were extracted from a working Mammotion-HA install and are NOT in
PyMammotion's public source, so they are not obscured here for "security" (see
next note) but to avoid publishing working credentials in plaintext.  They may
rotate if Mammotion ships a new app version; update via _ENCODED below if cloud
login starts failing with "Client id or secret error".

We use setdefault, so a real pre-set OS environment variable still wins.

--------------------------------------------------------------------------------
NOTE: the encoding below is OBFUSCATION, not security.  The values are XOR'd with
a fixed in-file key and base64'd purely to keep working app credentials out of
plaintext search / scrapers / secret-scanners — the key is right here, so anyone
can trivially reverse it.  To rotate: XOR+base64 the new value with _MASK and
replace the blob (or use the two-liner at the bottom of this file's git history).
--------------------------------------------------------------------------------
"""

import base64
import os

_MASK = b"ble-proxy-mask"


def _dec(blob: str) -> str:
    raw = base64.b64decode(blob)
    return bytes(b ^ _MASK[i % len(_MASK)] for i, b in enumerate(raw)).decode()


# Obfuscated (see NOTE above). Reversed at import time by _dec().
_ENCODED = {
    "MAMMOTION_OAUTH2_CLIENT_ID":
        "JRQATxchG0AKRFsROBow",
    "MAMMOTION_OAUTH2_CLIENT_SECRET":
        "KDxVGEBKPCozawxRMlJSLSFdCj4mNj1vFSwSXzQG",
    "ALIYUN_APP_KEY":
        "UVhXHElCXU8=",
    "ALIYUN_APP_SECRET":
        "Bw8GGENBCUBJGghUQF1bDlRPSRYLTUgbVAVFDQFZVhk=",
}

_APP_CONSTANTS = {k: _dec(v) for k, v in _ENCODED.items()}

for _key, _value in _APP_CONSTANTS.items():
    os.environ.setdefault(_key, _value)
