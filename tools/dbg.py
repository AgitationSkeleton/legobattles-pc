"""Client for the ndsrecomp debug server (newline-delimited JSON).

Reconnects once on a dropped socket: a long warp issues thousands of commands
and losing one of them should not throw away the run.
"""
import json, os, socket, sys, time

class Dbg:
    def __init__(self, host="127.0.0.1", port=19842, timeout=120.0):
        self.host, self.port, self.timeout = host, port, timeout
        self._open()

    def _open(self):
        self.s = socket.create_connection((self.host, self.port), timeout=self.timeout)
        self.f = self.s.makefile("rwb")

    def _once(self, payload):
        self.f.write((json.dumps(payload) + "\n").encode())
        self.f.flush()
        line = self.f.readline()
        if not line:
            raise RuntimeError("debug server closed the connection")
        try:
            return json.loads(line.decode("utf-8", "replace"))
        except Exception:
            return {"raw": line.decode("utf-8", "replace")[:400]}

    def cmd(self, name, strict=True, **kw):
        kw["cmd"] = name
        try:
            r = self._once(kw)
        except Exception:
            time.sleep(0.5)
            try:
                self.f.close(); self.s.close()
            except Exception:
                pass
            self._open()
            r = self._once(kw)
        # An {"error": ...} reply used to be returned like any other result, so a
        # refused command looked exactly like one that had run. That cost real
        # measurements: write_mem silently refused every write (it needs
        # NDS_DEBUG_ALLOW_WRITES=1, and rejects width=8), and the reads that
        # followed were read as "the game rewrote our value" rather than "the
        # write never happened". Failing loudly by default instead.
        if strict and isinstance(r, dict) and "error" in r:
            raise RuntimeError("debug server refused %s: %s" % (name, r["error"]))
        return r

    def close(self):
        try: self.f.close(); self.s.close()
        except Exception: pass


def assert_single_runner(port=19842):
    """Refuse to run while more than one emulator instance is alive.

    This has now cost measurements twice. The first time a leftover LEGO Star
    Wars II runner held the shared default port and this project drove THAT.
    The second time two LegoBattles.exe processes were up at once: commands went
    to one and screenshots came from the other, so a polygon count taken "in
    gameplay" was actually read from an instance sitting on the title screen --
    and an A/B experiment built on those readings produced a confident negative
    result that means nothing.

    Both were invisible at the time. The port answers either way; only the
    process count gives it away. Note the executable is deployed as
    LegoBattles.exe, so killing "nds_runner.exe" -- which is what was being
    done -- leaves the deployed instances running.
    """
    import re
    import subprocess

    names = ("nds_runner.exe", "LegoBattles.exe")
    try:
        out = subprocess.run(["tasklist"], capture_output=True, text=True,
                             timeout=30).stdout
    except Exception:                              # noqa: BLE001
        return                                     # cannot check; do not block
    live = [ln.split()[1] for ln in out.splitlines()
            if any(ln.lower().startswith(n.lower()) for n in names)]
    # Parallel checkouts (LB_HARNESS_OFFSET) each run their own: only the
    # instances started from THIS checkout can split a measurement here.
    try:
        import os
        root = os.path.normcase(os.path.abspath(
            os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")))
        wm = subprocess.run(
            ["wmic", "process", "where",
             "name='nds_runner.exe' or name='LegoBattles.exe'",
             "get", "ProcessId,ExecutablePath", "/format:csv"],
            capture_output=True, text=True, timeout=30).stdout
        mine = []
        for ln in wm.splitlines():
            parts = ln.strip().split(",")
            if len(parts) >= 3 and parts[-1].strip().isdigit():
                exe = os.path.normcase(os.path.abspath(",".join(parts[1:-1])))
                if exe.startswith(root + os.sep):
                    mine.append(parts[-1].strip())
        if wm.strip():
            live = [p for p in live if p in mine]
    except Exception:                              # noqa: BLE001
        pass
    if len(live) > 1:
        raise RuntimeError(
            "%d emulator instances are running (PIDs %s). Measurements split "
            "across instances are worthless -- kill them all first:\n"
            "    taskkill /F /IM LegoBattles.exe /IM nds_runner.exe"
            % (len(live), ", ".join(live)))

if __name__ == "__main__":
    d = Dbg()
    print(json.dumps(d.cmd(sys.argv[1], **(json.loads(sys.argv[2]) if len(sys.argv) > 2 else {})))[:2000])
