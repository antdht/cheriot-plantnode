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
    """

    timestamp: int
    temperature: float
    humidity: float
    moisture: int

    @classmethod
    def from_dict(cls, d: dict) -> "SensorPayload":
        return cls(
            timestamp=int(d["timestamp"]),
            temperature=float(d["temperature"]) / 10.0,
            humidity=float(d["humidity"]) / 10.0,
            moisture=int(d["moisture"]),
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
    connected_at: datetime.datetime = field(
        default_factory=datetime.datetime.now
    )
