# Charter

The Charter is the Python charter/plotter application that runs on a host machine alongside the CHERIoT PlantNode firmware. It:

- Connects to the MQTT broker over TLS (port 8883, server-authenticated only)
- Recovers per-device session keys via a Noise-N handshake (libhydrogen)
- Decrypts and displays real-time sensor telemetry (temperature, humidity, moisture)
- Can encrypt commands back to the device using the same session

## Requirements

Python 3.11+ is required. All dependencies are pinned in `requirements.txt`.

```bash
cd charter/
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

## Configuration

Copy the example config and fill in your values:

```bash
cp config.example.toml config.toml
```

Key fields in `config.toml`:

```toml
[mqtt]
host = "node.antdht.be"   # broker hostname
port = 8883               # TLS port
ca-cert = "./keys/ca.crt" # path to broker CA certificate (see Certificates below)
client-id = "charter"
topic-prefix = "plantnode"
```

## Certificates and keys

The `keys/` directory must contain three files before the charter can run:

| File | Description |
|------|-------------|
| `keys/ca.crt` | CA certificate of the MQTT broker (for TLS server verification) |
| `keys/verifier.pub` | Noise-N static public key (32 bytes) |
| `keys/verifier.key` | Noise-N static secret key (32 bytes) |

### Broker CA certificate

The charter connects to the broker with TLS but does **not** present a client certificate (unauthenticated from the broker's perspective). It only needs the broker's CA certificate to verify the server. Copy it from the broker's certificate directory:

```bash
cp ../mosquitto_broker/certs/ca.crt keys/ca.crt
```

The `ca-cert` path in `config.toml` is resolved relative to the working directory you launch the charter from (normally `charter/`), so `./keys/ca.crt` is the default.

### Verifier keypair

The Noise-N keypair is the identity of this verifier. The device firmware has the **public key** compiled in (`kVerifierPublicKey` in `src/crypto.cc`); the charter holds the matching **secret key** to complete the handshake and recover session keys.

To generate a fresh keypair (requires `cydrogen` to be installed):

```bash
python - <<'EOF'
import cydrogen, pathlib
kp = cydrogen.KxPair.gen()
pathlib.Path("keys/verifier.pub").write_bytes(bytes(kp.public_key()))
pathlib.Path("keys/verifier.key").write_bytes(bytes(kp.secret_key()))
print("Public key (paste into src/crypto.cc):")
print(", ".join(f"0x{b:02x}" for b in bytes(kp.public_key())))
EOF
```

After generating, update `kVerifierPublicKey` in `src/crypto.cc` with the printed bytes and rebuild the firmware.

> **Keep `keys/verifier.key` secret.** Anyone who holds it can decrypt all telemetry from any device that trusts this verifier. The `keys/` directory is listed in `.gitignore`.

## Running

```bash
cd charter/
source .venv/bin/activate
python main.py
```

A window opens with three live plots (temperature, humidity, moisture). The charter subscribes to `{topic-prefix}/keys/#` for session key exchange and `{topic-prefix}/telemetry` for encrypted sensor data.

Session key recovery is automatic: the device publishes its Noise-N packet1 as a **retained** message, so the charter recovers session keys immediately on connect even if the device is not currently active.
