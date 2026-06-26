import datetime
from dataclasses import dataclass, field


@dataclass
class SensorPayload:
    """
    Decrypted sensor reading from a PlantNode device.

    Values are converted to human-readable units from the raw firmware format:
      - temperature: degrees C  (firmware sends × 10, divided here)
      - humidity:    %RH        (firmware sends × 10, divided here)
      - moisture:    raw Seesaw capacitance (dry ≈ 400, wet ≈ 1800)
      - timestamp:   UNIX epoch seconds
      - last_watering: UNIX epoch seconds of the most recent pump activation
                       (0 if the device has never watered)
    """

    timestamp: int
    temperature: float
    humidity: float
    moisture: int
    last_watering: int

    @classmethod
    def from_dict(cls, d: dict) -> "SensorPayload":
        return cls(
            timestamp=int(d["timestamp"]),
            temperature=float(d["temperature"]) / 10.0,
            humidity=float(d["humidity"]) / 10.0,
            moisture=int(d["moisture"]),
            last_watering=int(d.get("lastWatering", 0)),
        )


@dataclass
class SensorSession:
    """
    Active encryption session for a single PlantNode device.
    Created when a Noise-N packet1 is received on the key topic.
    """

    device_id: str
    rx_key: bytes
    tx_key: bytes
    tx_msg_id: int = 0
    # Most recent last_watering value seen in telemetry; None until the first
    # payload is processed, so a watering is only reported once observed to
    # change.
    last_watering_seen: int | None = None
    connected_at: datetime.datetime = field(default_factory=datetime.datetime.now)
