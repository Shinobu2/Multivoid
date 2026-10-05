#!/usr/bin/env python3
"""release_sign -- signs and verifies a main.dll with Ed25519, standard library only.

    python -I -B .github/ci/release_sign.py sign   --dll <path> --target <t> --build <n> --out <path> [--test-key]
    python -I -B .github/ci/release_sign.py verify --dll <path> --sig <path> [--trust-test-key]

`sign` writes the six-line `.sig` file the game parses (`coop/build_trust`): the DLL's SHA-256, the
build it belongs to and an Ed25519 signature over `signed_message`. `verify` is the same judgement
the game makes, in the same order, and prints which step refused.

The job that holds the release key installs nothing, so the signature scheme is written here from
the algorithm of RFC 8032 section 5.1 (field arithmetic, point encoding and decoding, extended
coordinate addition, key generation, sign, verify) and checked against its section 7.1 vectors by
`release_sign_drill.py`. Python's big integers do the field work.

This is NOT constant-time: it signs one DLL per release on a CI runner. The seed is read from the
environment only, and no message and no exception text this file writes holds it.

Exit codes: 0 done; 1 verification failed; 2 a usage or key-table error; 3 the environment (the seed).
Every message is one line on stdout and starts `release_sign: `.
"""
import argparse
import hashlib
import os
import pathlib
import re
import sys

# The selftest key: its seed is public by design, it signs only rig and drill builds, and the game
# trusts it only where a developer switch is set. It is never a row of the release table.
TEST_SEED_HEX = "920aa36f330a7de4743b96ac4b67cb724338a15c6a1aecbd1d23e8d377477af1"
TEST_KEY_ID = 255
SEED_ENV = "MULTIVOID_RELEASE_KEY"

_MAX_BUILD = 4294967295
_MESSAGE_TAG = b"multivoid-build-v1"

# --- Ed25519 ------------------------------------------------------------------------------------

_P = 2**255 - 19
_L = 2**252 + 27742317777372353535851937790883648493
_D = (-121665 * pow(121666, _P - 2, _P)) % _P
_SQRT_M1 = pow(2, (_P - 1) // 4, _P)
_Y_MASK = (1 << 255) - 1

# A point is (X, Y, Z, T) in extended homogeneous coordinates: x = X/Z, y = Y/Z, x*y = T/Z.
_NEUTRAL = (0, 1, 1, 0)


def _add(p, q):
    x1, y1, z1, t1 = p
    x2, y2, z2, t2 = q
    a = (y1 - x1) * (y2 - x2) % _P
    b = (y1 + x1) * (y2 + x2) % _P
    c = t1 * 2 * _D * t2 % _P
    d = z1 * 2 * z2 % _P
    e, f, g, h = b - a, d - c, d + c, b + a
    return (e * f % _P, g * h % _P, f * g % _P, e * h % _P)


def _double(p):
    x1, y1, z1, _ = p
    a = x1 * x1 % _P
    b = y1 * y1 % _P
    c = 2 * z1 * z1 % _P
    h = a + b
    e = h - (x1 + y1) * (x1 + y1)
    g = a - b
    f = c + g
    return (e * f % _P, g * h % _P, f * g % _P, e * h % _P)


def _mul(k, p):
    """[k]p, most significant bit first."""
    acc = _NEUTRAL
    for bit in range(k.bit_length() - 1, -1, -1):
        acc = _double(acc)
        if (k >> bit) & 1:
            acc = _add(acc, p)
    return acc


def _same(p, q):
    return ((p[0] * q[2] - q[0] * p[2]) % _P == 0
            and (p[1] * q[2] - q[1] * p[2]) % _P == 0)


def _encode(p):
    zi = pow(p[2], _P - 2, _P)
    x = p[0] * zi % _P
    y = p[1] * zi % _P
    return (y | ((x & 1) << 255)).to_bytes(32, "little")


def _decode(data):
    """The point a 32-byte string encodes, or None when it encodes none."""
    if len(data) != 32:
        return None
    raw = int.from_bytes(data, "little")
    x_odd = raw >> 255
    y = raw & _Y_MASK
    if y >= _P:
        return None
    yy = y * y % _P
    u = (yy - 1) % _P
    v = (_D * yy + 1) % _P
    # x = (u/v)^((p+3)/8), computed with one modular power for the inversion and the root.
    x = u * pow(v, 3, _P) * pow(u * pow(v, 7, _P), (_P - 5) // 8, _P) % _P
    check = v * x * x % _P
    if check == (-u) % _P:
        x = x * _SQRT_M1 % _P
    elif check != u:
        return None
    if x == 0 and x_odd:
        return None
    if (x & 1) != x_odd:
        x = _P - x
    return (x, y, 1, x * y % _P)


_BASE = _decode((4 * pow(5, _P - 2, _P)).to_bytes(32, "little"))


def _digest_int(*parts):
    return int.from_bytes(hashlib.sha512(b"".join(parts)).digest(), "little")


def _expand(seed):
    """(secret scalar, prefix, public key) of a 32-byte seed."""
    if len(seed) != 32:
        raise ValueError("an Ed25519 seed is 32 bytes")
    h = hashlib.sha512(seed).digest()
    s = int.from_bytes(h[:32], "little")
    s = (s & ((1 << 254) - 8)) | (1 << 254)
    return s, h[32:], _encode(_mul(s, _BASE))


def ed25519_public(seed):
    return _expand(seed)[2]


def ed25519_sign(seed, msg):
    s, prefix, pub = _expand(seed)
    r = _digest_int(prefix, msg) % _L
    r_enc = _encode(_mul(r, _BASE))
    k = _digest_int(r_enc, pub, msg) % _L
    return r_enc + ((r + k * s) % _L).to_bytes(32, "little")


def ed25519_verify(pub, msg, sig):
    """True only for a valid signature; a bad point, a bad length or a wrong type is False."""
    if not (isinstance(pub, (bytes, bytearray)) and isinstance(msg, (bytes, bytearray))
            and isinstance(sig, (bytes, bytearray))):
        return False
    pub, msg, sig = bytes(pub), bytes(msg), bytes(sig)
    if len(pub) != 32 or len(sig) != 64:
        return False
    a = _decode(pub)
    r = _decode(sig[:32])
    s = int.from_bytes(sig[32:], "little")
    if a is None or r is None or s >= _L:
        return False
    k = _digest_int(sig[:32], pub, msg) % _L
    return _same(_mul(8, _mul(s, _BASE)), _mul(8, _add(r, _mul(k, a))))


# --- the message and the .sig file --------------------------------------------------------------


class SignError(Exception):
    """A refusal with its step name and exit code; str(e) is the whole output line."""

    def __init__(self, step, code, message):
        super().__init__(message)
        self.step = step
        self.code = code


def _verify_failed(step):
    return SignError(step, 1, "release_sign: verify failed at " + step)


def signed_message(target, build, sha):
    """Byte for byte coop::build_trust::SignedMessage: tag, length, target, u32le build, sha."""
    t = target.encode("ascii")
    return _MESSAGE_TAG + bytes([len(t)]) + t + build.to_bytes(4, "little") + sha


def format_sig(key_id, target, build, sha, sig):
    return ("multivoid-build-sig 1\nkey %d\ntarget %s\nbuild %d\nsha256 %s\nsig %s\n"
            % (key_id, target, build, sha.hex(), sig.hex()))


_SIG_LINES = (
    re.compile(r"multivoid-build-sig 1", re.ASCII),
    re.compile(r"key ([1-9][0-9]{0,2})", re.ASCII),
    re.compile(r"target ([\x21-\x7e]{1,23})", re.ASCII),
    re.compile(r"build (0|[1-9][0-9]{0,9})", re.ASCII),
    re.compile(r"sha256 ([0-9a-f]{64})", re.ASCII),
    re.compile(r"sig ([0-9a-f]{128})", re.ASCII),
)


def parse_sig(text):
    """The game's grammar: six lines in order, "\\n" or "\\r\\n" ends, a final newline optional."""
    lines = text.split("\n")
    if lines[-1] == "":
        del lines[-1]
    if len(lines) != len(_SIG_LINES):
        raise _verify_failed("grammar")
    values = []
    for line, rule in zip(lines, _SIG_LINES):
        if line.endswith("\r"):
            line = line[:-1]
        m = rule.fullmatch(line)
        if m is None:
            raise _verify_failed("grammar")
        values.append(m.group(1) if m.groups() else None)
    key_id = int(values[1])
    build = int(values[3])
    if key_id > 255 or build > _MAX_BUILD:
        raise _verify_failed("grammar")
    return {"key_id": key_id, "target": values[2], "build": build,
            "sha": bytes.fromhex(values[4]), "sig": bytes.fromhex(values[5])}


# --- the key table ------------------------------------------------------------------------------

_ROW = re.compile(r'MV_RELEASE_KEY\(([1-9][0-9]{0,2}), "([0-9a-f]{64})", "([0-9a-f]{128})"\)',
                  re.ASCII)


def inc_path():
    return (pathlib.Path(__file__).resolve().parents[2]
            / "src/votv-coop/include/coop/build_trust/release_keys.inc")


def format_row(key_id, pub, fixture_sig):
    return 'MV_RELEASE_KEY(%d, "%s", "%s")' % (key_id, pub.hex(), fixture_sig.hex())


def read_rows(text):
    """[(id, pub_hex, fixture_sig_hex)] of the table text; the game's compile-time checks, here."""
    rows = []
    seen = set()
    for number, line in enumerate(text.split("\n"), start=1):
        if line.endswith("\r"):
            line = line[:-1]
        if line == "" or line.startswith("//"):
            continue
        m = _ROW.fullmatch(line)
        if m is None or not 1 <= int(m.group(1)) <= 254:
            raise SignError("table", 2,
                            "release_sign: release_keys.inc line %d is not a row" % number)
        key_id = int(m.group(1))
        if key_id in seen:
            raise SignError("table", 2, "release_sign: release_keys.inc repeats id %d" % key_id)
        seen.add(key_id)
        rows.append((key_id, m.group(2), m.group(3)))
    return rows


def _table_rows():
    path = inc_path()
    if not path.is_file():
        raise SignError("table", 2, "release_sign: release_keys.inc not found at the checkout")
    return read_rows(path.read_bytes().decode("utf-8"))


# --- the commands -------------------------------------------------------------------------------


def load_seed(environ):
    value = environ.get(SEED_ENV)
    if value is None:
        raise SignError("seed", 3, "release_sign: %s is missing" % SEED_ENV)
    if re.fullmatch(r"[0-9a-f]{64}", value, re.ASCII) is None:
        raise SignError("seed", 3, "release_sign: %s is not 64 lowercase hex characters" % SEED_ENV)
    return bytes.fromhex(value)


def _file_sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.digest()


def cmd_sign(args, environ, out):
    if args.test_key:
        seed = bytes.fromhex(TEST_SEED_HEX)
        key_id = TEST_KEY_ID
    else:
        seed = load_seed(environ)
        pub_hex = ed25519_public(seed).hex()
        matches = [row[0] for row in _table_rows() if row[1] == pub_hex]
        if not matches:
            raise SignError("key", 2, "release_sign: the key is not in release_keys.inc")
        key_id = matches[0]

    target = args.target
    if re.fullmatch(r"[\x21-\x7e]{1,23}", target, re.ASCII) is None:
        raise SignError("usage", 2,
                        "release_sign: --target must be 1 to 23 printable ASCII characters "
                        "without a space")
    build_text = str(args.build)
    if re.fullmatch(r"0|[1-9][0-9]{0,9}", build_text, re.ASCII) is None or \
            int(build_text) > _MAX_BUILD:
        raise SignError("usage", 2, "release_sign: --build must be a number from 0 to 4294967295")
    build = int(build_text)

    try:
        sha = _file_sha256(args.dll)
    except OSError:
        raise SignError("read", 2, "release_sign: cannot read --dll") from None
    sig = ed25519_sign(seed, signed_message(target, build, sha))
    try:
        with open(args.out, "w", encoding="ascii", newline="\n") as f:
            f.write(format_sig(key_id, target, build, sha, sig))
    except OSError:
        raise SignError("write", 2, "release_sign: cannot write --out") from None
    out.write("release_sign: signed sha %s with key %d\n" % (sha.hex()[:8], key_id))
    return 0


def cmd_verify(args, out):
    try:
        sig_bytes = pathlib.Path(args.sig).read_bytes()
        sha = _file_sha256(args.dll)
    except OSError:
        raise _verify_failed("read") from None
    try:
        text = sig_bytes.decode("ascii")
    except UnicodeDecodeError:
        raise _verify_failed("grammar") from None
    parsed = parse_sig(text)
    if parsed["sha"] != sha:
        raise _verify_failed("sha")

    pub = None
    for key_id, pub_hex, _fixture in _table_rows():
        if key_id == parsed["key_id"]:
            pub = bytes.fromhex(pub_hex)
    if pub is None and args.trust_test_key and parsed["key_id"] == TEST_KEY_ID:
        pub = ed25519_public(bytes.fromhex(TEST_SEED_HEX))
    if pub is None:
        raise _verify_failed("key")
    msg = signed_message(parsed["target"], parsed["build"], parsed["sha"])
    if not ed25519_verify(pub, msg, parsed["sig"]):
        raise _verify_failed("signature")
    out.write("release_sign: verified key %d\n" % parsed["key_id"])
    return 0


def _parser():
    parser = argparse.ArgumentParser(prog="release_sign", allow_abbrev=False)
    sub = parser.add_subparsers(dest="command", required=True)
    s = sub.add_parser("sign", allow_abbrev=False)
    s.add_argument("--dll", required=True)
    s.add_argument("--target", required=True)
    s.add_argument("--build", required=True)
    s.add_argument("--out", required=True)
    s.add_argument("--test-key", action="store_true")
    v = sub.add_parser("verify", allow_abbrev=False)
    v.add_argument("--dll", required=True)
    v.add_argument("--sig", required=True)
    v.add_argument("--trust-test-key", action="store_true")
    return parser


def main(argv, environ=None, out=None):
    environ = os.environ if environ is None else environ
    out = sys.stdout if out is None else out
    args = _parser().parse_args(argv)
    try:
        if args.command == "sign":
            return cmd_sign(args, environ, out)
        return cmd_verify(args, out)
    except SignError as e:
        out.write(str(e) + "\n")
        return e.code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
