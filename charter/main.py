import datetime
import secrets
import tkinter
from dataclasses import dataclass, field
from queue import Queue
from typing import Optional, TypedDict

import paho.mqtt.client as mqtt
from paho.mqtt import client as mqtt_lib

from config import AppConfig
from attestation import (
    RA_CHALLENGE1,
    RA_CHALLENGE2,
    RA_NONCE_REPLY,
    RA_QUOTE,
    RA_APPROVED,
    NONCE_LENGTH,
    combine_nonce,
    deserialize_quote,
    verify_quote,
)
from encryption import (
    load_verifier_keypair,
    recover_session_keys,
    decrypt_telemetry,
    decrypt_message,
    encrypt_command,
)
from logger import CsvLogger
from plotter import Plotter, PlotterQueue
from sensors import SensorPayload, SensorSession

import cydrogen


@dataclass
class AppState:
    """Application state containing shared resources."""

    temperature_queue: Queue[PlotterQueue]
    humidity_queue: Queue[PlotterQueue]
    moisture_queue: Queue[PlotterQueue]
    config: AppConfig
    logger: CsvLogger
    verifier_kp: cydrogen.KxPair
    sessions: dict[str, SensorSession] = field(default_factory=dict)
    mqtt_client: Optional[mqtt_lib.Client] = None


class MqttUserData(TypedDict):
    state: AppState


def enqueue_for_plots(sender: str, payload: SensorPayload, state: AppState) -> None:
    ts = datetime.datetime.fromtimestamp(float(payload.timestamp))
    state.temperature_queue.put({"sender": sender, "x": ts, "y": payload.temperature})
    state.humidity_queue.put({"sender": sender, "x": ts, "y": payload.humidity})
    state.moisture_queue.put({"sender": sender, "x": ts, "y": payload.moisture})


def _attestation_topic(state: AppState) -> str:
    return f"{state.config.mqtt.topic_prefix}/attestation"


def send_ra_message(
    state: AppState, session: SensorSession, payload: bytes
) -> None:
    """Encrypt an RA handshake message under the session TX key and publish it to
    plantnode/attestation (the device decrypts with its RX key)."""
    if state.mqtt_client is None:
        return
    wire = encrypt_command(session.tx_key, session.tx_msg_id, payload)
    session.tx_msg_id += 1
    state.mqtt_client.publish(_attestation_topic(state), wire, qos=1)


def initiate_attestation(state: AppState, session: SensorSession) -> None:
    """Start a dual-nonce handshake: pick a fresh verifier nonce and send
    challenge 1."""
    nonce_v = secrets.token_bytes(NONCE_LENGTH)
    session.ra_nonce_v = nonce_v
    session.ra_combined = None
    send_ra_message(state, session, bytes([RA_CHALLENGE1]) + nonce_v)
    print(
        f"[attestation] {session.device_id}: sent challenge 1 "
        f"(nonce_V={nonce_v.hex()[:16]}...)"
    )


def handle_attestation_message(
    state: AppState, session: SensorSession, raw: bytes
) -> None:
    """Process a message on plantnode/attestation. The verifier's own echoed
    challenges (encrypted under the TX key) fail to decrypt and are skipped."""
    try:
        plain = decrypt_message(session.rx_key, raw)
    except (cydrogen.DecryptException, ValueError):
        # Our own challenge echoed back (wrong key direction) — ignore.
        return

    if len(plain) < 1:
        return
    msg_type = plain[0]
    payload = plain[1:]

    if msg_type == RA_NONCE_REPLY:
        if len(payload) != 2 * NONCE_LENGTH:
            print(f"[attestation] bad nonce reply length ({len(payload)})")
            return
        echoed_v = payload[:NONCE_LENGTH]
        nonce_d = payload[NONCE_LENGTH:]
        if session.ra_nonce_v is None or echoed_v != session.ra_nonce_v:
            print("[attestation] nonce reply does not echo our nonce_V — ignoring")
            return
        combined = combine_nonce(session.ra_nonce_v, nonce_d)
        session.ra_combined = combined
        send_ra_message(state, session, bytes([RA_CHALLENGE2]) + combined)
        print(
            f"[attestation] {session.device_id}: got nonce reply "
            f"(nonce_D={nonce_d.hex()[:16]}...), sent challenge 2 "
            f"(combined={combined.hex()[:16]}...)"
        )

    elif msg_type == RA_QUOTE:
        if session.ra_combined is None:
            print("[attestation] quote received with no pending handshake — ignoring")
            return
        try:
            quote = deserialize_quote(payload)
        except ValueError as e:
            print(f"[attestation] malformed quote: {e}")
            return
        ok, reasons = verify_quote(quote, session.ra_combined)
        device_id = quote["device_id"].decode("ascii", errors="replace")
        verdict = "OK" if ok else "FAILED"
        print(f"[attestation] quote from '{device_id}' slot {quote['slot']}: {verdict}")
        for reason in reasons:
            print(f"    - {reason}")
        if ok:
            # Approve the device: it withholds telemetry until it sees this.
            send_ra_message(
                state, session, bytes([RA_APPROVED]) + session.ra_combined
            )
            print(f"[attestation] {session.device_id}: approval sent")
        else:
            print(
                f"[attestation] {session.device_id}: quote rejected — "
                f"no approval sent, device telemetry stays withheld"
            )
        session.ra_nonce_v = None
        session.ra_combined = None

    else:
        print(f"[attestation] unknown message type {msg_type}")


def on_connect(
    client: mqtt.Client, userdata: MqttUserData, flags, rc, properties
) -> None:
    if rc != 0:
        print("Failed to connect, return code:", rc)
        return

    prefix = userdata["state"].config.mqtt.topic_prefix
    print(
        f"Connected (rc={rc}), subscribing to {prefix}/keys/#, "
        f"{prefix}/telemetry and {prefix}/attestation"
    )
    client.subscribe(f"{prefix}/keys/#")
    client.subscribe(f"{prefix}/telemetry")
    client.subscribe(f"{prefix}/attestation")


def on_message(
    client: mqtt.Client, userdata: MqttUserData, message: mqtt.MQTTMessage
) -> None:
    state = userdata["state"]
    topic = message.topic
    payload_bytes = message.payload
    prefix = state.config.mqtt.topic_prefix

    try:
        # ── Key distribution message: plantnode/keys/{device_id} ─────────
        if topic.startswith(f"{prefix}/keys/"):
            device_id = topic.split("/")[-1]
            if len(payload_bytes) != 48:
                print(
                    f"Key packet from {device_id} has wrong length "
                    f"({len(payload_bytes)} bytes, expected 48)"
                )
                return

            rx_key, tx_key = recover_session_keys(state.verifier_kp, payload_bytes)
            session = SensorSession(
                device_id=device_id, rx_key=rx_key, tx_key=tx_key
            )
            state.sessions[device_id] = session
            print(
                f"Session established with {device_id} "
                f"(rx_key={rx_key.hex()[:16]}... "
                f"tx_key={tx_key.hex()[:16]}...)"
            )
            # Kick off a remote-attestation handshake now that we can talk to it.
            initiate_attestation(state, session)

        # ── Encrypted telemetry: plantnode/telemetry ──────────────────────
        elif topic == f"{prefix}/telemetry":
            # Binary format: [8B msg_id LE][secretbox ciphertext]
            # The device_id is compiled into the key topic; for single-device
            # setups use the first (and only) active session.
            if not state.sessions:
                print("No active session yet, cannot decrypt telemetry")
                return

            # Support single device: use the first session
            # (extend with per-sender routing when multiple devices are needed)
            device_id, session = next(iter(state.sessions.items()))

            data = decrypt_telemetry(session.rx_key, payload_bytes)
            reading = SensorPayload.from_dict(data)

            print(
                f"[{device_id}] T={reading.temperature:.1f}°C "
                f"H={reading.humidity:.1f}%RH "
                f"M={reading.moisture} "
                f"ts={reading.timestamp}"
            )

            # Report a watering only when the timestamp changes from the last
            # one seen for this device (the first payload just sets a baseline).
            if (
                session.last_watering_seen is not None
                and reading.last_watering != session.last_watering_seen
            ):
                watered_at = datetime.datetime.fromtimestamp(
                    float(reading.last_watering)
                )
                print(f"[{device_id}] New watering detected at {watered_at}")
            session.last_watering_seen = reading.last_watering

            state.logger.log(device_id, reading)
            enqueue_for_plots(device_id, reading, state)

        # ── Remote-attestation handshake: plantnode/attestation ───────────
        elif topic == f"{prefix}/attestation":
            if not state.sessions:
                # No session yet — can't decrypt; skip (also covers our own echo).
                return
            device_id, session = next(iter(state.sessions.items()))
            handle_attestation_message(state, session, bytes(payload_bytes))

    except cydrogen.DecryptException as e:
        print(f"Decryption failed on topic {topic}: {e}")
    except Exception as e:
        print(f"Unexpected error processing message on {topic}: {e}")


def init_mqtt(state: AppState) -> mqtt.Client:
    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        state.config.mqtt.client_id,
        userdata={"state": state},
    )

    client.tls_set(ca_certs=state.config.mqtt.ca_cert)

    client.on_message = on_message
    client.on_connect = on_connect
    client.connect(state.config.mqtt.host, state.config.mqtt.port, 60)
    return client


def init_tkinter(client: mqtt.Client) -> tkinter.Tk:
    root = tkinter.Tk()
    root.title("PlantNode — Real-time Sensor Data")

    def on_closing() -> None:
        client.loop_stop()
        client.disconnect()
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_closing)
    return root


def init_plotters(tk_root: tkinter.Tk, state: AppState) -> tuple[Plotter, ...]:
    left_frame = tkinter.Frame(tk_root)
    mid_frame = tkinter.Frame(tk_root)
    right_frame = tkinter.Frame(tk_root)
    left_frame.pack(side="left", fill="both", expand=True)
    mid_frame.pack(side="left", fill="both", expand=True)
    right_frame.pack(side="left", fill="both", expand=True)

    interval = state.config.plotter.refresh_interval
    max_pts = state.config.plotter.max_points

    temp_plot = Plotter(
        left_frame,
        state.temperature_queue,
        interval=interval,
        y_label="Temperature (°C)",
        title="Temperature Over Time",
        max_points=max_pts,
    )
    humidity_plot = Plotter(
        mid_frame,
        state.humidity_queue,
        interval=interval,
        y_label="Humidity (%RH)",
        title="Humidity Over Time",
        max_points=max_pts,
    )
    moisture_plot = Plotter(
        right_frame,
        state.moisture_queue,
        interval=interval,
        y_label="Moisture (raw)",
        title="Moisture Over Time",
        max_points=max_pts,
    )
    return temp_plot, humidity_plot, moisture_plot


def main() -> None:
    config = AppConfig()
    verifier_kp = load_verifier_keypair()
    print(f"Verifier pk: {bytes(verifier_kp.public_key()).hex()}")

    state = AppState(
        temperature_queue=Queue(),
        humidity_queue=Queue(),
        moisture_queue=Queue(),
        config=config,
        logger=CsvLogger(config.logger.csv_file_name),
        verifier_kp=verifier_kp,
    )

    client = init_mqtt(state)
    state.mqtt_client = client

    tk_root = init_tkinter(client)
    init_plotters(tk_root, state)

    client.loop_start()
    tk_root.mainloop()

    client.loop_stop()
    client.disconnect()


if __name__ == "__main__":
    main()
