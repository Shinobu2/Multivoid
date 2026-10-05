#!/usr/bin/env python3
"""release_sign_drill_lib -- what the two release signer drills share.

`release_sign_drill.py` (the signature arms) and `release_sign_keygen_drill.py` (the key's
custody arms) run under `python -I -B`, which leaves the script's folder off sys.path, so each
loads this file by its path, the way this file loads `release_sign.py`. Nothing here runs by itself.
"""
import base64
import contextlib
import importlib.util
import pathlib

HERE = pathlib.Path(__file__).resolve().parent

_spec = importlib.util.spec_from_file_location("release_sign", HERE / "release_sign.py")
rs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rs)

RESULTS = []


def check(name, ok):
    RESULTS.append(bool(ok))
    print(("PASS " if ok else "FAIL ") + name)


def write_ascii(path, text):
    """Bytes, never write_text(newline=): that parameter needs Python 3.10, the floor is 3.9."""
    path.write_bytes(text.encode("ascii"))


@contextlib.contextmanager
def table_at(path):
    original = rs.inc_path
    rs.inc_path = lambda: path
    try:
        yield
    finally:
        rs.inc_path = original


def leaks(seed, text):
    hexa = seed.hex()
    forms = [hexa, hexa.upper(), repr(seed), base64.b64encode(seed).decode().rstrip("="),
             base64.urlsafe_b64encode(seed).decode().rstrip("=")]
    return [f for f in forms if f in text]


def guarded(group, *args):
    """A group that dies on an exception it did not expect is a FAIL line, not a traceback."""
    try:
        group(*args)
    except Exception as e:
        check("%s raised %s" % (group.__name__, type(e).__name__), False)


def finish(title):
    if all(RESULTS):
        print("%s: ALL PASS (%d checks)" % (title, len(RESULTS)))
        return 0
    print("%s: %d of %d checks FAILED" % (title, RESULTS.count(False), len(RESULTS)))
    return 1
