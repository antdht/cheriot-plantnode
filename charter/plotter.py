# Heavily inspired by https://github.com/smaroukis/realtime-plotter
import datetime
import tkinter as tk
from queue import Queue
from typing import TypedDict, Optional
import hashlib
import colorsys

import matplotlib.dates as mdates
import matplotlib.pyplot as plt
import mplcursors
from matplotlib.animation import FuncAnimation
from matplotlib.backends.backend_tkagg import (FigureCanvasTkAgg)


class PlotterQueue(TypedDict):
    sender: int
    x: datetime.datetime
    y: float


class Plotter:
    """
    Embed a real-time line plot into a Tkinter container.

    The plot consumes points from a queue where each item is a mapping with
    "sender", "x", and "y" keys representing the data point's coordinates.
    "x" is expected to be a datetime.datetime instance, and "y" a float.

    New points are pulled from the queue at each animation interval and
    appended to the plotted time series. Multiple senders are supported with
    different colors for each sender.
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
    ) -> None:
        """
        Initialize the Plotter instance.

        :param master: Tkinter container to embed the plot into.
        :param queue: Queue from which to pull incoming data points.
        :param interval: Animation update interval in milliseconds.
        :param x_label: Label for the X axis.
        :param y_label: Label for the Y axis.
        :param title: Optional title for the plot.
        :param max_points: Maximum number of points to retain in the plot.
        """

        self.queue: Queue[PlotterQueue] = queue
        self.max_points = max_points

        # Store data per sender
        self.sender_data: dict[int, dict[str, list]] = {}
        # Store line objects per sender
        self.sender_lines: dict[int, plt.Line2D] = {}
        # Store colors per sender
        self.sender_colors: dict[int, str] = {}

        self.fig, self.ax = plt.subplots()

        # Initialize empty plot
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

        # Keep a reference to the animation object on self to avoid GC
        self.ani = FuncAnimation(self.fig, self._update, interval=interval)

        # Initialize cursor (will be updated in _update)
        self.cursor = None

    def _on_hover(self, sel) -> None:
        """
        Callback for hover events on data points.
        Shows sender information along with time and value.

        :param sel: The selection object from mplcursors.
        """
        if sel.target is None or sel.index is None:
            return
            
        # Find which sender this point belongs to by checking the line
        sender_id = None
        for sid, line in self.sender_lines.items():
            if line == sel.artist:
                sender_id = sid
                break
                
        if sender_id is not None:
            x_num = sel.target[0]
            y_val = sel.target[1]
            x_datetime = mdates.num2date(x_num)
            
            sel.annotation.set_text(
                f"Sender {sender_id}\n"
                f"Time: {x_datetime:%Y-%m-%d %H:%M:%S}\n"
                f"Value: {y_val:.2f}"
            )
        else:
            # Fallback if sender not found
            x_num = sel.target[0]
            y_val = sel.target[1]
            x_datetime = mdates.num2date(x_num)
            
            sel.annotation.set_text(
                f"Time: {x_datetime:%Y-%m-%d %H:%M:%S}\n"
                f"Value: {y_val:.2f}"
            )

    def _get_color_for_sender(self, sender_id: int) -> str:
        """
        Get a consistent color for a sender ID.
        Uses a deterministic color assignment based on sender ID that ensures
        good visual distinction between different senders.

        :param sender_id: The sender ID.
        :return: A color string.
        """
        if sender_id not in self.sender_colors:
            # Use a deterministic color assignment based on sender ID
            # This ensures the same sender always gets the same color
            import hashlib
            import colorsys
            
            # Create a hash of the sender ID to get a consistent color
            hash_obj = hashlib.md5(str(sender_id).encode())
            hash_int = int(hash_obj.hexdigest(), 16)
            
            # Improved color assignment for better visual distinction
            # Use a more sophisticated approach to ensure colors are visually distinct
            
            # Calculate hue with better spacing - use golden ratio for distribution
            golden_ratio = (1 + 5**0.5) / 2  # ~1.618
            hue = (hash_int * golden_ratio) % 1.0
            
            # Ensure good saturation and brightness for visibility
            saturation = 0.8 + (hash_int % 50) / 500.0  # 0.8-0.9 range
            value = 0.7 + (hash_int % 100) / 1000.0  # 0.7-0.8 range
            
            # Convert HSV to RGB
            rgb = colorsys.hsv_to_rgb(hue, saturation, value)
            
            # Convert to hex color
            color = f"#{int(rgb[0]*255):02x}{int(rgb[1]*255):02x}{int(rgb[2]*255):02x}"
            
            # Improved approach: use a larger, more diverse color palette
            # with better hue distribution to ensure visual distinction
            
            # Use golden ratio for better hue distribution
            golden_ratio = (1 + 5**0.5) / 2  # ~1.618
            hue = (hash_int * golden_ratio) % 1.0
            
            # Use higher saturation and value for better visibility
            saturation = 0.85 + (hash_int % 30) / 300.0  # 0.85-0.95 range
            value = 0.75 + (hash_int % 40) / 400.0  # 0.75-0.85 range
            
            # Convert HSV to RGB
            rgb = colorsys.hsv_to_rgb(hue, saturation, value)
            
            # Convert to hex color
            color = f"#{int(rgb[0]*255):02x}{int(rgb[1]*255):02x}{int(rgb[2]*255):02x}"
            
            # If we already have colors, ensure this one is sufficiently different
            # by checking against existing colors and adjusting if needed
            if self.sender_colors:
                max_attempts = 10
                attempt = 0
                original_hue = hue
                
                while attempt < max_attempts:
                    # Check if this color is too similar to any existing color
                    too_similar = False
                    rgb_current = (rgb[0], rgb[1], rgb[2])
                    
                    for existing_color in self.sender_colors.values():
                        # Convert hex to RGB
                        hex_color = existing_color.lstrip('#')
                        rgb_existing = tuple(int(hex_color[i:i+2], 16) / 255.0 for i in (0, 2, 4))
                        
                        # Calculate improved color distance using weighted RGB
                        # Weight red and green more heavily as human vision is more sensitive to them
                        distance = ((rgb_current[0] - rgb_existing[0])**2 * 0.3 + 
                                  (rgb_current[1] - rgb_existing[1])**2 * 0.6 + 
                                  (rgb_current[2] - rgb_existing[2])**2 * 0.1)**0.5
                        
                        # Also check hue difference for better perceptual distinction
                        h_current, s_current, v_current = colorsys.rgb_to_hsv(rgb_current[0], rgb_current[1], rgb_current[2])
                        h_existing, s_existing, v_existing = colorsys.rgb_to_hsv(rgb_existing[0], rgb_existing[1], rgb_existing[2])
                        hue_diff = min(abs(h_current - h_existing), 1.0 - abs(h_current - h_existing))
                        
                        # If colors are too similar in both RGB and hue, mark for adjustment
                        if distance < 0.2 or hue_diff < 0.1:
                            too_similar = True
                            break
                    
                    # If color is sufficiently different, we're done
                    if not too_similar:
                        break
                    
                    # Otherwise, adjust hue significantly and try again
                    hue = (hue + 0.25) % 1.0  # Shift hue by 25%
                    rgb = colorsys.hsv_to_rgb(hue, saturation, value)
                    color = f"#{int(rgb[0]*255):02x}{int(rgb[1]*255):02x}{int(rgb[2]*255):02x}"
                    attempt += 1
                
                # If we exhausted attempts, use a fallback approach with more distinct hues
                if attempt >= max_attempts:
                    # Use a simple hue rotation with larger steps
                    hue = (len(self.sender_colors) * 0.618) % 1.0  # Golden ratio spacing
                    rgb = colorsys.hsv_to_rgb(hue, 0.9, 0.8)
                    color = f"#{int(rgb[0]*255):02x}{int(rgb[1]*255):02x}{int(rgb[2]*255):02x}"
            
            self.sender_colors[sender_id] = color
            
        return self.sender_colors[sender_id]

    def __del__(self) -> None:
        plt.ioff()

    def store_data(self, sender_id: int, x: datetime.datetime, y: float) -> None:
        """
        Append a single (x, y) point to the internal series for a specific sender.

        If max_points is set, only keep the most recent max_points values.

        :param sender_id: The sender ID.
        :param x: X coordinate (datetime).
        :param y: Y coordinate (float).
        """

        # Initialize sender data if not exists
        if sender_id not in self.sender_data:
            self.sender_data[sender_id] = {
                "x": [],
                "y": []
            }

        # Store the data point
        self.sender_data[sender_id]["x"].append(x)
        self.sender_data[sender_id]["y"].append(y)

        # Apply max_points limit if set
        if (self.max_points is not None) and (len(self.sender_data[sender_id]["x"]) > self.max_points):
            self.sender_data[sender_id]["x"] = self.sender_data[sender_id]["x"][-self.max_points:]
            self.sender_data[sender_id]["y"] = self.sender_data[sender_id]["y"][-self.max_points:]

    def _update(self, frame: int) -> list[plt.Line2D]:
        """
        Animation update function called at each interval by FuncAnimation.

        :param frame: Frame number.
        :return: List containing the updated line objects.
        """

        # Process new data points from the queue
        while not self.queue.empty():
            point = self.queue.get()
            if not ("sender" in point and "x" in point and "y" in point):
                continue
            sender = int(point["sender"])
            x = point["x"]
            y = float(point["y"])
            self.store_data(sender, x, y)

        # Clear existing lines
        for line in self.lines:
            line.remove()
        self.lines.clear()
        self.sender_lines.clear()

        # Draw lines for each sender
        updated_lines = []
        for sender_id, data in self.sender_data.items():
            if data["x"] and data["y"]:
                xs = mdates.date2num(data["x"])
                color = self._get_color_for_sender(sender_id)
                
                if sender_id in self.sender_lines:
                    # Update existing line
                    line = self.sender_lines[sender_id]
                    line.set_data(xs, data["y"])
                    line.set_color(color)
                else:
                    # Create new line
                    (line,) = self.ax.plot(xs, data["y"], lw=1.5, marker="o", markersize=3, color=color, label=f"Sender {sender_id}")
                    self.sender_lines[sender_id] = line
                
                self.lines.append(line)
                updated_lines.append(line)

        if updated_lines:
            self.ax.relim()
            self.ax.autoscale_view()
            # Add legend if there are multiple senders
            if len(self.sender_data) > 1:
                self.ax.legend(loc='upper right')
            
            # Update cursor for hover tooltips
            if self.cursor is not None:
                self.cursor.remove()
            self.cursor = mplcursors.cursor(self.lines, hover=True)
            self.cursor.connect("add", self._on_hover)
            
            self.canvas.draw_idle()

        return updated_lines
