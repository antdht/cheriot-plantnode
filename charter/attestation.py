"""Mock attestation verifier.

The focus has moved off real remote attestation. The signed-quote, dual-nonce,
image-hash-verification protocol has been removed and replaced by a plaintext
JSON mock API on plantnode/attestation.

The device publishes a query carrying an opaque token (deliberately abstract —
no slot/measurement/signature, just "here is my evidence"):

    {"query": "am_i_attested", "device": "plantnode-001", "token": "<hex>"}

and this module produces the response:

    {"attested": true, "device": "plantnode-001"}

This is a MOCK: it always attests. It logs the claimed token so the exchange
looks plausible, but performs NO verification.
"""

import json
from typing import Optional

# First-byte/field marker of a device attestation query.
ATTESTATION_QUERY = "am_i_attested"


def parse_request(raw: bytes) -> Optional[dict]:
    """Parse a device JSON message. Return the dict only if it is an
    `am_i_attested` query; otherwise None (this also filters out our own
    response echoed back on the shared topic and any junk)."""
    try:
        msg = json.loads(raw)
    except (ValueError, UnicodeDecodeError):
        return None
    if not isinstance(msg, dict) or msg.get("query") != ATTESTATION_QUERY:
        return None
    return msg


def build_response(request: dict) -> bytes:
    """MOCK verdict: always attested. Echoes the device id back so the device
    can match it. Returns the JSON response as bytes ready to publish."""
    device = request.get("device", "unknown")
    return json.dumps({"attested": True, "device": device}).encode()
