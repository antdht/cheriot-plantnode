"""Build and send remote threshold-update commands to a plantnode device."""

import time

from encryption import encrypt_command

# Advisory-only tracking of the last timestamp sent per device, so we can warn
# the operator when a same-second send is likely to be rejected as stale by
# the firmware (see send_threshold_update). Keyed like state.sessions.
_last_sent_timestamp: dict[str, int] = {}


def build_threshold_command(timestamp: int, threshold: int) -> bytes:
    """Build the fixed-order plaintext JSON payload the firmware expects.

    Field order matters: the firmware parser is a fixed-format scanner, not
    a general JSON parser, and only accepts
    {"timestamp":<uint32>,"threshold":<uint16>} in exactly this key order.
    A general-purpose json.dumps() is deliberately not used here, since it
    does not guarantee this key order.
    """
    return f'{{"timestamp":{timestamp},"threshold":{threshold}}}'.encode()


def send_threshold_update(state, threshold: int) -> None:
    """Encrypt and publish a threshold-update command to the active device.

    No-op (with a printed message) if no session has been established yet.
    Fire-and-forget: no acknowledgment is expected back from the device.
    """
    if not state.sessions:
        print("[commands] No active session yet, cannot send threshold update")
        return
    if state.mqtt_client is None:
        print("[commands] No MQTT client, cannot send threshold update")
        return

    # Single active device, same lookup pattern used for telemetry decrypt.
    device_id, session = next(iter(state.sessions.items()))

    timestamp = int(time.time())
    if timestamp <= _last_sent_timestamp.get(device_id, -1):
        print(
            "[commands] Warning: sending within the same second as the last "
            "command — the device will likely reject this one as stale"
        )
    _last_sent_timestamp[device_id] = timestamp

    plaintext = build_threshold_command(timestamp, threshold)
    ciphertext = encrypt_command(session.tx_key, session.tx_msg_id, plaintext)
    session.tx_msg_id += 1

    prefix = state.config.mqtt.topic_prefix
    state.mqtt_client.publish(f"{prefix}/commands", ciphertext, qos=1)
    print(
        f"[commands] Sent threshold={threshold} (ts={timestamp}) to {device_id}"
    )
