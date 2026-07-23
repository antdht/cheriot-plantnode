# Heavily inspired by https://github.com/smaroukis/realtime-plotter
import colorsys
import datetime
import hashlib
import tkinter as tk
from queue import Queue
from typing import TypedDict, Optional

import matplotlib.dates as mdates
import matplotlib.pyplot as plt
import mplcursors
from matplotlib.animation import FuncAnimation
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg


class PlotterQueue(TypedDict):
    sender: str
    x: datetime.datetime
    y: float


class PlotterEvent(TypedDict):
    sender: str
    x: datetime.datetime


class Plotter:
    """
    Embed a real-time line plot into a Tkinter container.

    The plot consumes points from a queue where each item is a mapping with
    "sender", "x", and "y" keys. "x" is a datetime, "y" a float, "sender" a
    string device ID. Multiple senders are supported with distinct colours.
    """

    def __init__(
        self,
        master: tk.Misc,
        queue: Queue[PlotterQueue],
        interval: int = 1000,
        x_label: str = "Time",
        y_label: str = "Value",
        title: Optional[str] = None,
        max_points: Optional[int] = None,
        event_queue: Optional[Queue[PlotterEvent]] = None,
        event_color: str = "darkblue",
    ) -> None:
        self.queue: Queue[PlotterQueue] = queue
        self.max_points = max_points

        self.sender_data: dict[str, dict[str, list]] = {}
        self.sender_lines: dict[str, plt.Line2D] = {}
        self.sender_colors: dict[str, str] = {}

        # Optional vertical marker lines (e.g. "watering happened here"),
        # drawn once per event and never removed/redrawn afterwards.
        self.event_queue: Optional[Queue[PlotterEvent]] = event_queue
        self.event_color = event_color
        self.drawn_event_count: dict[str, int] = {}
        self.event_times: dict[str, list[datetime.datetime]] = {}

        self.fig, self.ax = plt.subplots()
        self.lines: list[plt.Line2D] = []

        self.ax.xaxis_date()
        self.ax.xaxis.set_major_locator(mdates.AutoDateLocator())
        self.ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M:%S"))
        self.fig.autofmt_xdate()

        self.ax.set_xlabel(x_label)
        self.ax.set_ylabel(y_label)
        if title:
            self.ax.set_title(title)

        self.ax.margins(y=0.1)
        self.fig.tight_layout()

        self.canvas = FigureCanvasTkAgg(self.fig, master)
        self.canvas.get_tk_widget().pack(fill="both", expand=True)

        self.ani = FuncAnimation(
            self.fig, self._update, interval=interval, cache_frame_data=False
        )
        self.cursor = None

    def _on_hover(self, sel) -> None:
        if sel.target is None or sel.index is None:
            return

        sender_id = None
        for sid, line in self.sender_lines.items():
            if line == sel.artist:
                sender_id = sid
                break

        x_datetime = mdates.num2date(sel.target[0])
        y_val = sel.target[1]
        label = f"Sender {sender_id}\n" if sender_id is not None else ""
        sel.annotation.set_text(
            f"{label}Time: {x_datetime:%Y-%m-%d %H:%M:%S}\nValue: {y_val:.2f}"
        )

    def _get_color_for_sender(self, sender_id: str) -> str:
        if sender_id not in self.sender_colors:
            if self.sender_colors:
                # Golden-ratio hue spacing from the number of already-assigned colours
                hue = (len(self.sender_colors) * 0.618) % 1.0
            else:
                # Deterministic hue from a hash of the sender string
                h = int(hashlib.md5(sender_id.encode()).hexdigest(), 16)
                hue = (h * ((1 + 5**0.5) / 2)) % 1.0

            rgb = colorsys.hsv_to_rgb(hue, 0.85, 0.80)
            self.sender_colors[sender_id] = (
                f"#{int(rgb[0]*255):02x}{int(rgb[1]*255):02x}{int(rgb[2]*255):02x}"
            )

        return self.sender_colors[sender_id]

    def store_data(self, sender_id: str, x: datetime.datetime, y: float) -> None:
        if sender_id not in self.sender_data:
            self.sender_data[sender_id] = {"x": [], "y": []}

        self.sender_data[sender_id]["x"].append(x)
        self.sender_data[sender_id]["y"].append(y)

        if self.max_points is not None:
            d = self.sender_data[sender_id]
            if len(d["x"]) > self.max_points:
                d["x"] = d["x"][-self.max_points :]
                d["y"] = d["y"][-self.max_points :]

    def _draw_new_event_lines(self) -> None:
        if self.event_queue is None:
            return

        while not self.event_queue.empty():
            event = self.event_queue.get()
            if not ("sender" in event and "x" in event):
                continue
            self.event_times.setdefault(event["sender"], []).append(event["x"])

        for sender_id, times in self.event_times.items():
            drawn = self.drawn_event_count.get(sender_id, 0)
            for x in times[drawn:]:
                self.ax.axvline(
                    mdates.date2num(x),
                    color=self.event_color,
                    linestyle="--",
                    linewidth=1.5,
                    zorder=0,
                )
            self.drawn_event_count[sender_id] = len(times)

    def _update(self, frame: int) -> list[plt.Line2D]:
        while not self.queue.empty():
            point = self.queue.get()
            if not ("sender" in point and "x" in point and "y" in point):
                continue
            self.store_data(point["sender"], point["x"], float(point["y"]))

        self._draw_new_event_lines()

        for line in self.lines:
            line.remove()
        self.lines.clear()
        self.sender_lines.clear()

        updated_lines = []
        for sender_id, data in self.sender_data.items():
            if data["x"] and data["y"]:
                xs = mdates.date2num(data["x"])
                color = self._get_color_for_sender(sender_id)

                if sender_id in self.sender_lines:
                    line = self.sender_lines[sender_id]
                    line.set_data(xs, data["y"])
                    line.set_color(color)
                else:
                    (line,) = self.ax.plot(
                        xs,
                        data["y"],
                        lw=1.5,
                        marker="o",
                        markersize=3,
                        color=color,
                        label=f"Sender {sender_id}",
                    )
                    self.sender_lines[sender_id] = line

                self.lines.append(line)
                updated_lines.append(line)

        if updated_lines:
            self.ax.relim()
            self.ax.autoscale_view()
            if len(self.sender_data) > 1:
                self.ax.legend(loc="upper right")

            if self.cursor is not None:
                self.cursor.remove()
            self.cursor = mplcursors.cursor(self.lines, hover=True)
            self.cursor.connect("add", self._on_hover)

            self.canvas.draw_idle()

        return updated_lines
