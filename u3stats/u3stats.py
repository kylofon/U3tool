"""
u3stats.py -- a live stats and inventory window for Ultima III: Exodus.

Reads the party block straight out of the running DOSBox process, so the
display follows the game in real time without touching the save files.

Run it with:  pythonw u3stats.py     (or use "Ultima 3 Stats.bat")
"""

from __future__ import annotations

import os
import queue
import sys
import threading
import time
import tkinter as tk
from tkinter import font as tkfont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import u3reader as R

POLL_SECONDS = 0.25

BG = "#17130d"
PANEL = "#241d14"
PANEL_EDGE = "#3b3020"
FG = "#e9dcc2"
DIM = "#94856b"
GOLD = "#d8a657"
GREEN = "#7fa650"
AMBER = "#d0a020"
RED = "#c05038"
GREY = "#6a6055"

STATUS_COLOURS = {"Good": GREEN, "Poisoned": AMBER, "Dead": RED, "Ashes": GREY}


def num(v, dash="-"):
    return dash if v is None else str(v)


class CharPanel(tk.Frame):
    """One character column. Widgets are built once and only re-texted."""

    def __init__(self, master, fonts):
        super().__init__(master, bg=PANEL, highlightbackground=PANEL_EDGE,
                         highlightthickness=1, padx=10, pady=8)
        self.fonts = fonts

        self.name = tk.Label(self, text="", font=fonts["name"], fg=GOLD,
                             bg=PANEL, anchor="w")
        self.name.pack(fill="x")

        self.kind = tk.Label(self, text="", font=fonts["small"], fg=DIM,
                             bg=PANEL, anchor="w")
        self.kind.pack(fill="x")

        self.status = tk.Label(self, text="", font=fonts["boldsmall"],
                               fg=GREEN, bg=PANEL, anchor="w")
        self.status.pack(fill="x", pady=(4, 2))

        self.bar = tk.Canvas(self, height=14, bg=PANEL, highlightthickness=0)
        self.bar.pack(fill="x")
        self._bar_bg = self.bar.create_rectangle(
            0, 0, 0, 14, fill="#3a2f20", outline="")
        self._bar_fg = self.bar.create_rectangle(
            0, 0, 0, 14, fill=GREEN, outline="")
        self._bar_text = self.bar.create_text(
            4, 7, anchor="w", text="", fill=FG, font=fonts["tiny"])

        self.mp = tk.Label(self, text="", font=fonts["body"], fg=FG, bg=PANEL,
                           anchor="w")
        self.mp.pack(fill="x", pady=(3, 0))

        self._rule()

        stats = tk.Frame(self, bg=PANEL)
        stats.pack(fill="x")
        self.stat_labels = {}
        for i, key in enumerate(("Str", "Dex", "Int", "Wis")):
            r, c = divmod(i, 2)
            cell = tk.Frame(stats, bg=PANEL)
            cell.grid(row=r, column=c, sticky="w", padx=(0, 14))
            tk.Label(cell, text=key, font=fonts["tiny"], fg=DIM,
                     bg=PANEL).pack(side="left")
            lab = tk.Label(cell, text="", font=fonts["boldbody"], fg=FG,
                           bg=PANEL, width=3, anchor="e")
            lab.pack(side="left")
            self.stat_labels[key] = lab

        self._rule()

        self.rows = {}
        for key in ("Exp", "Food", "Gold"):
            self.rows[key] = self._row(key)

        self._rule()

        self.rows["Weapon"] = self._row("Weapon", value_colour=GOLD)
        self.rows["Armour"] = self._row("Armour", value_colour=GOLD)

        self._rule()

        tk.Label(self, text="CARRYING", font=fonts["tiny"], fg=DIM, bg=PANEL,
                 anchor="w").pack(fill="x")
        self.pack_list = tk.Label(self, text="", font=fonts["small"], fg=FG,
                                  bg=PANEL, anchor="nw", justify="left",
                                  wraplength=200)
        self.pack_list.pack(fill="x", pady=(1, 0))

        self.counters = tk.Label(self, text="", font=fonts["small"], fg=FG,
                                 bg=PANEL, anchor="nw", justify="left",
                                 wraplength=200)
        self.counters.pack(fill="x", pady=(6, 0))

    def _rule(self):
        tk.Frame(self, bg=PANEL_EDGE, height=1).pack(fill="x", pady=6)

    def _row(self, label, value_colour=FG):
        row = tk.Frame(self, bg=PANEL)
        row.pack(fill="x")
        tk.Label(row, text=label, font=self.fonts["small"], fg=DIM,
                 bg=PANEL, anchor="w", width=7).pack(side="left")
        val = tk.Label(row, text="", font=self.fonts["body"], fg=value_colour,
                       bg=PANEL, anchor="e")
        val.pack(side="right")
        return val

    # ----------------------------------------------------------------
    def show_empty(self):
        self.name.config(text="- empty -", fg=GREY)
        self.kind.config(text="")
        self.status.config(text="")
        self.mp.config(text="")
        self.bar.itemconfig(self._bar_text, text="")
        self.bar.coords(self._bar_fg, 0, 0, 0, 14)
        for lab in self.stat_labels.values():
            lab.config(text="")
        for val in self.rows.values():
            val.config(text="")
        self.pack_list.config(text="")
        self.counters.config(text="")

    def update_from(self, c):
        self.name.config(text=c.name or "(unnamed)", fg=GOLD)
        self.kind.config(text="%s %s | %s" % (c.race, c.klass, c.sex))

        colour = STATUS_COLOURS.get(c.status, AMBER)
        self.status.config(text=c.status.upper(), fg=colour)

        hp, mx = c.hp, c.max_hp
        width = max(self.bar.winfo_width(), 1)
        self.bar.coords(self._bar_bg, 0, 0, width, 14)
        if hp is not None and mx:
            frac = max(0.0, min(1.0, hp / mx))
            fill = GREEN if frac > 0.5 else (AMBER if frac > 0.25 else RED)
            self.bar.coords(self._bar_fg, 0, 0, width * frac, 14)
            self.bar.itemconfig(self._bar_fg, fill=fill)
            self.bar.itemconfig(self._bar_text, text="HP  %d / %d" % (hp, mx))
        else:
            self.bar.coords(self._bar_fg, 0, 0, 0, 14)
            self.bar.itemconfig(self._bar_text, text="HP  ?")

        self.mp.config(text="MP  %s" % num(c.mp))

        for key, v in (("Str", c.strength), ("Dex", c.dexterity),
                       ("Int", c.intelligence), ("Wis", c.wisdom)):
            self.stat_labels[key].config(text=num(v))

        self.rows["Exp"].config(text=num(c.exp))
        self.rows["Food"].config(text=num(c.food))
        self.rows["Gold"].config(text=num(c.gold))
        self.rows["Weapon"].config(text=c.weapon)
        self.rows["Armour"].config(text=c.armour)

        items = ["%s x%d" % (n, q) for n, q in c.weapons_owned]
        items += ["%s x%d" % (n, q) for n, q in c.armour_owned]
        self.pack_list.config(text="\n".join(items) if items else "nothing")

        self.counters.config(text="Gems %s   Keys %s\nPowders %s   Torches %s"
                             % (num(c.gems), num(c.keys), num(c.powders),
                                num(c.torches)))


class Poller(threading.Thread):
    """Keeps the reader work off the UI thread -- a full scan takes a moment."""

    daemon = True

    def __init__(self, out_queue):
        super().__init__()
        self.q = out_queue
        self.reader = R.DosBoxReader()
        self.rescan = threading.Event()
        self.next_source = threading.Event()
        self.stop = threading.Event()

    def run(self):
        while not self.stop.is_set():
            if self.rescan.is_set():
                self.rescan.clear()
                if self.reader.attach():
                    self.reader.scan()
            if self.next_source.is_set():
                self.next_source.clear()
                self.reader.next_candidate()
            try:
                party = self.reader.poll()
            except Exception as exc:                      # keep the UI alive
                party = None
                self.reader.last_error = "read error: %s" % exc
            self.q.put((party, self.reader.last_error, self.reader.exe,
                        self.reader.pid, self.reader.address,
                        len(self.reader.candidates), self.reader.index))
            time.sleep(POLL_SECONDS)


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Ultima III - Party Stats")
        self.configure(bg=BG)
        self.geometry("1060x680")
        self.minsize(880, 560)

        base = "Consolas" if "Consolas" in tkfont.families() else "Courier New"
        self.fonts = {
            "name": tkfont.Font(family=base, size=13, weight="bold"),
            "boldbody": tkfont.Font(family=base, size=10, weight="bold"),
            "boldsmall": tkfont.Font(family=base, size=9, weight="bold"),
            "body": tkfont.Font(family=base, size=10),
            "small": tkfont.Font(family=base, size=9),
            "tiny": tkfont.Font(family=base, size=8),
        }

        bar = tk.Frame(self, bg=BG, padx=10, pady=6)
        bar.pack(fill="x")

        self.status_label = tk.Label(bar, text="Looking for DOSBox...",
                                     font=self.fonts["small"], fg=DIM, bg=BG,
                                     anchor="w")
        self.status_label.pack(side="left", fill="x", expand=True)

        self.topmost = tk.BooleanVar(value=True)
        self.attributes("-topmost", True)
        tk.Checkbutton(bar, text="always on top", variable=self.topmost,
                       command=self._toggle_top, font=self.fonts["small"],
                       fg=DIM, bg=BG, selectcolor=PANEL, activebackground=BG,
                       activeforeground=FG, borderwidth=0,
                       highlightthickness=0).pack(side="right", padx=4)

        self.show_raw = tk.BooleanVar(value=False)
        tk.Checkbutton(bar, text="raw bytes", variable=self.show_raw,
                       command=self._toggle_raw, font=self.fonts["small"],
                       fg=DIM, bg=BG, selectcolor=PANEL, activebackground=BG,
                       activeforeground=FG, borderwidth=0,
                       highlightthickness=0).pack(side="right", padx=4)

        for text, cmd in (("next source", self._next_source),
                          ("rescan", self._rescan)):
            tk.Button(bar, text=text, command=cmd, font=self.fonts["small"],
                      fg=FG, bg=PANEL, activebackground=PANEL_EDGE,
                      activeforeground=FG, relief="flat", padx=8
                      ).pack(side="right", padx=4)

        body = tk.Frame(self, bg=BG, padx=8, pady=2)
        body.pack(fill="both", expand=True)
        self.panels = []
        for i in range(4):
            body.columnconfigure(i, weight=1, uniform="chars")
            p = CharPanel(body, self.fonts)
            p.grid(row=0, column=i, sticky="nsew", padx=4, pady=4)
            self.panels.append(p)
        body.rowconfigure(0, weight=1)

        self.raw_frame = tk.Frame(self, bg=BG, padx=12, pady=6)
        self.raw_text = tk.Text(self.raw_frame, height=12, bg="#100d08",
                                fg=DIM, insertbackground=FG,
                                font=self.fonts["tiny"], relief="flat",
                                highlightthickness=1,
                                highlightbackground=PANEL_EDGE)
        self.raw_text.pack(fill="both", expand=True)

        self.queue = queue.Queue()
        self.poller = Poller(self.queue)
        self.poller.start()

        self.protocol("WM_DELETE_WINDOW", self._close)
        self.after(100, self._drain)

    # ----------------------------------------------------------------
    def _toggle_top(self):
        self.attributes("-topmost", self.topmost.get())

    def _toggle_raw(self):
        if self.show_raw.get():
            self.raw_frame.pack(fill="both", expand=False)
        else:
            self.raw_frame.pack_forget()

    def _rescan(self):
        self.status_label.config(text="Scanning DOSBox memory...")
        self.poller.rescan.set()

    def _next_source(self):
        self.poller.next_source.set()

    def _close(self):
        self.poller.stop.set()
        self.destroy()

    # ----------------------------------------------------------------
    def _drain(self):
        latest = None
        try:
            while True:
                latest = self.queue.get_nowait()
        except queue.Empty:
            pass
        if latest:
            self._render(*latest)
        self.after(120, self._drain)

    def _render(self, party, error, exe, pid, address, n_cands, index):
        if party is None:
            self.status_label.config(
                text=error or "Waiting for a party in memory...", fg=AMBER)
            for p in self.panels:
                p.show_empty()
            return

        where = "%s (pid %s) @ 0x%X" % (exe or "DOSBox", pid, address or 0)
        if n_cands > 1:
            where += "   [source %d of %d]" % (index + 1, n_cands)
        self.status_label.config(
            text="Live - %d in party - %s" % (party.count, where), fg=GREEN)

        for i, panel in enumerate(self.panels):
            c = party.characters[i]
            if i < party.count and c.present:
                panel.update_from(c)
            else:
                panel.show_empty()

        if self.show_raw.get():
            self.raw_text.delete("1.0", "end")
            self.raw_text.insert("1.0", hexdump(party.raw))

    # ----------------------------------------------------------------


def hexdump(data):
    lines = ["party header (18 bytes) then 4 x 64-byte character records",
             ""]
    for off in range(0, len(data), 16):
        chunk = data[off:off + 16]
        hexpart = " ".join("%02x" % b for b in chunk)
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        if off < R.HEADER_SIZE:
            tag = "hdr"
        else:
            rec, within = divmod(off - R.HEADER_SIZE, R.RECORD_SIZE)
            tag = "c%d+%02x" % (rec, within)
        lines.append("%04x  %-7s  %-47s  %s" % (off, tag, hexpart, text))
    return "\n".join(lines)


if __name__ == "__main__":
    App().mainloop()
