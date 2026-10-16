"""The LEGO Battles master server, GUI flavour.

The master server comes as two executables: a cmd instance that shows logs and
keeps them, and a GUI variant with a sidepanel listing lobbies, each expandable
to show the connected players under it. This is that second one. It runs
the same `MasterServer` in a thread, so the two flavours are the same service
with a different face - there is no second implementation to drift.

    python master_server_gui.py --port 28555

LAYOUT. A sidebar on the left holding the lobby tree: each lobby is a row that
expands to show its connected players. The right and larger portion is the
console log, the same lines the cmd flavour prints, appended live. Right-click a
lobby to close it; right-click a player or lobby to ban the address it came
from. That mirrors the LSW2 master server's kick/ban affordance, which exists
for the same reason: somebody has to be able to remove a malicious host without
restarting the service.

TKINTER, because it ships with Python. The cmd flavour has no GUI dependency at
all, so a headless operator never pays for this file.
"""

from __future__ import annotations

import argparse
import os
import queue
import sys
import threading
import time

import master_server
from master_server import DEFAULT_MAX_PLAYERS, DEFAULT_PORT, TTL_SECONDS

try:
    import tkinter as tk
    from tkinter import messagebox, ttk
except ImportError:  # pragma: no cover - headless box
    tk = None


REFRESH_MS = 1000          # the lobby tree; TTL is 90s so this is plenty
PUMP_MS = 120              # the log queue


class Gui:
    def __init__(self, root, srv, store, log):
        self.root = root
        self.srv = srv
        self.store = store
        self.log = log
        self.queue = queue.Queue()
        self.open_codes = set()

        root.title("LEGO Battles - master server")
        root.geometry("1100x640")
        root.minsize(760, 420)

        paned = ttk.PanedWindow(root, orient="horizontal")
        paned.pack(fill="both", expand=True)

        # -- left: the lobby sidepanel --
        left = ttk.Frame(paned, width=340)
        paned.add(left, weight=0)

        head = ttk.Frame(left)
        head.pack(fill="x", padx=6, pady=(6, 0))
        ttk.Label(head, text="Lobbies", font=("", 10, "bold")).pack(side="left")
        self.count = ttk.Label(head, text="0")
        self.count.pack(side="right")

        self.tree = ttk.Treeview(left, show="tree", selectmode="browse")
        self.tree.pack(fill="both", expand=True, padx=6, pady=6)
        self.tree.bind("<<TreeviewOpen>>", self._remember_open)
        self.tree.bind("<<TreeviewClose>>", self._forget_open)
        self.tree.bind("<Button-3>", self._context)

        # -- right: the console --
        right = ttk.Frame(paned)
        paned.add(right, weight=1)

        bar = ttk.Frame(right)
        bar.pack(fill="x", padx=6, pady=(6, 0))
        ttk.Label(bar, text="Console", font=("", 10, "bold")).pack(side="left")
        self.status = ttk.Label(bar, text="")
        self.status.pack(side="right")

        wrap = ttk.Frame(right)
        wrap.pack(fill="both", expand=True, padx=6, pady=6)
        self.text = tk.Text(wrap, wrap="none", state="disabled",
                            background="#111318", foreground="#d6d9df",
                            insertbackground="#d6d9df", font=("Consolas", 9))
        sb = ttk.Scrollbar(wrap, orient="vertical", command=self.text.yview)
        self.text.configure(yscrollcommand=sb.set)
        sb.pack(side="right", fill="y")
        self.text.pack(side="left", fill="both", expand=True)

        self.menu = tk.Menu(root, tearoff=0)
        self.menu.add_command(label="Close lobby", command=self._close_lobby)
        self.menu.add_command(label="Ban address", command=self._ban)
        self.menu.add_separator()
        self.menu.add_command(label="Copy address", command=self._copy)

        for line in self.log.subscribe(self.queue.put):
            self._append(line)

        self.root.after(PUMP_MS, self._pump)
        self.root.after(0, self._refresh)

    # -- log --

    def _append(self, line):
        self.text.configure(state="normal")
        at_end = self.text.yview()[1] > 0.999
        self.text.insert("end", line + "\n")
        # keep the widget bounded; the file on disk is the real record
        if int(self.text.index("end-1c").split(".")[0]) > 2000:
            self.text.delete("1.0", "500.0")
        if at_end:
            self.text.see("end")
        self.text.configure(state="disabled")

    def _pump(self):
        try:
            while True:
                self._append(self.queue.get_nowait())
        except queue.Empty:
            pass
        self.root.after(PUMP_MS, self._pump)

    # -- lobby tree --

    def _remember_open(self, _evt):
        item = self.tree.focus()
        if item:
            self.open_codes.add(item)

    def _forget_open(self, _evt):
        item = self.tree.focus()
        self.open_codes.discard(item)

    def _refresh(self):
        now = time.time()
        self.srv.sweep(now)
        lobbies = self.store.lobbies(now)
        live = {lob["code"] for lob in lobbies}

        for item in self.tree.get_children(""):
            if item not in live:
                self.tree.delete(item)
                self.open_codes.discard(item)

        for lob in lobbies:
            code = lob["code"]
            age = int(now - lob["seen"])
            label = ("%s  %s   %d/%d   %s:%d   %ds ago"
                     % (code, lob["name"], lob["players"], lob["max_players"],
                        lob["address"], lob["port"], age))
            if self.tree.exists(code):
                self.tree.item(code, text=label)
                for child in self.tree.get_children(code):
                    self.tree.delete(child)
            else:
                self.tree.insert("", "end", iid=code, text=label, open=False)

            players = self.store.players(code)
            if players:
                for p in players:
                    self.tree.insert(
                        code, "end",
                        text="    %d. %s   %s" % (p["slot"] + 1, p["name"],
                                                  p["address"] or "-"))
            else:
                self.tree.insert(code, "end",
                                 text="    host: %s" % lob["host_name"])
            if code in self.open_codes:
                self.tree.item(code, open=True)

        self.count.configure(text=str(len(lobbies)))
        self.status.configure(
            text="ttl %ds   ceiling %d players   %d banned"
                 % (TTL_SECONDS, self.srv.max_players, len(self.store.bans())))
        self.root.after(REFRESH_MS, self._refresh)

    # -- context menu --

    def _selected_lobby(self):
        item = self.tree.focus()
        if not item:
            return None
        parent = self.tree.parent(item)
        return parent or item

    def _context(self, evt):
        item = self.tree.identify_row(evt.y)
        if item:
            self.tree.selection_set(item)
            self.tree.focus(item)
            self.menu.tk_popup(evt.x_root, evt.y_root)

    def _address_of(self, code):
        lob = self.store.lobby(code)
        return lob["address"] if lob else None

    def _close_lobby(self):
        code = self._selected_lobby()
        if not code:
            return
        if self.store.remove(code):
            self.log("ADMIN", "operator closed lobby %s" % code)

    def _ban(self):
        code = self._selected_lobby()
        if not code:
            return
        addr = self._address_of(code)
        if not addr:
            return
        if messagebox.askyesno("Ban address",
                               "Ban %s and close its lobbies?" % addr):
            self.store.ban(addr, "banned from the master server GUI")
            for lob in self.store.lobbies():
                if lob["address"] == addr:
                    self.store.remove(lob["code"])
            self.log("ADMIN", "operator banned %s" % addr)

    def _copy(self):
        code = self._selected_lobby()
        addr = self._address_of(code) if code else None
        if addr:
            self.root.clipboard_clear()
            self.root.clipboard_append(addr)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--host", default="")
    ap.add_argument("--db", default="master.sqlite3")
    ap.add_argument("--log", default=os.path.join("logs", "master.log"))
    ap.add_argument("--max-players", type=int, default=DEFAULT_MAX_PLAYERS)
    args = ap.parse_args(argv)

    if tk is None:
        print("tkinter is not available; use master_server.py for the "
              "console flavour", file=sys.stderr)
        return 2

    # echo=False: the GUI shows the lines, and duplicating them to a console
    # that may not exist is noise. The file sink stays on.
    srv, store, log = master_server.build(
        args.port, args.db, args.log, echo=False,
        max_players=args.max_players, host=args.host)

    thread = threading.Thread(target=srv.serve_forever, daemon=True)
    thread.start()
    log("START", "LEGO Battles master server (GUI) on %s:%d"
        % (args.host or "0.0.0.0", args.port))
    log("START", "database %s, log %s, ttl %ds, operator ceiling %d players"
        % (args.db, args.log, TTL_SECONDS, args.max_players))

    root = tk.Tk()
    Gui(root, srv, store, log)
    try:
        root.mainloop()
    finally:
        log("STOP", "GUI closed")
        srv.shutdown()
        srv.server_close()
        store.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
