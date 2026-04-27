import csv
import datetime
import os

from sensors import SensorPayload

CSV_HEADER = ["date", "sender", "temperature", "humidity", "moisture"]


class CsvLogger:
    """Used to log sensor readings to a CSV file"""

    def __init__(self, filename: str) -> None:
        """
        Initialize the Logger instance.
        Creates the CSV file with header if it does not exist.
        :param filename: Path to the CSV file to log data.
        :raises OSError: If the file cannot be created or accessed.
        """
        self.filename = filename

        if not self.filename.lower().endswith(".csv"):
            self.filename += ".csv"

        if not os.path.exists(self.filename):
            with open(self.filename, mode="w", newline="") as file:
                writer = csv.writer(file)
                writer.writerow(CSV_HEADER)

    def log(self, sender: str, payload: SensorPayload) -> None:
        """
        Append a sensor reading to the CSV file.
        :param sender: Identifier of the device sending the data.
        :param payload: Decrypted SensorPayload.
        :raises OSError: If the file cannot be opened for writing.
        """
        try:
            with open(self.filename, mode="a", newline="") as file:
                writer = csv.writer(file)
                writer.writerow(
                    [
                        datetime.datetime.fromtimestamp(payload.timestamp).isoformat(),
                        sender,
                        payload.temperature,
                        payload.humidity,
                        payload.moisture,
                    ]
                )
        except OSError as e:
            print(f"Error writing to log file {self.filename}: {e}")
