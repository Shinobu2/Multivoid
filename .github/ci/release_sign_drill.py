#!/usr/bin/env python3
"""release_sign_drill -- proves the release signer accepts what it must and refuses the rest.

Standard library only; every vector is embedded and every file it makes lives in a temporary
directory. A refusal arm is a check that FAILs when the signer ACCEPTS what it must refuse. The
throwaway seed of the release-path arms comes from os.urandom and is never printed.

Run: python -I -B .github/ci/release_sign_drill.py
"""
import argparse
import base64
import contextlib
import importlib.util
import io
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent

_spec = importlib.util.spec_from_file_location("release_sign", HERE / "release_sign.py")
rs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rs)

RESULTS = []


def check(name, ok):
    RESULTS.append(bool(ok))
    print(("PASS " if ok else "FAIL ") + name)


def flip_first_bit(data):
    return bytes([data[0] ^ 1]) + data[1:]


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


# RFC 8032 section 7.1, tests 1 to 3: (secret, public, message, signature).
RFC_VECTORS = [
    ("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
     "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
     "",
     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e3970"
     "1cf9b46bd25bf5f0595bbe24655141438e7a100b"),
    ("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
     "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
     "72",
     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613"
     "d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"),
    ("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
     "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
     "af82",
     "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760"
     "984dc6594a7c15e9716ed28dc027beceea1ec40a"),
]

# The build-trust vector: the test seed signs target 0.9.0n, build 216, over this DLL hash.
VEC_SEED = bytes.fromhex(rs.TEST_SEED_HEX)
VEC_TARGET = "0.9.0n"
VEC_BUILD = 216
VEC_SHA_HEX = "5c445d2907e03e872fcc2d0b0725ec5a438cca603825c0ba93058f534facc837"
VEC_MESSAGE_HEX = ("6d756c7469766f69642d6275696c642d763106302e392e306ed80000005c445d2907e03e872fcc"
                   "2d0b0725ec5a438cca603825c0ba93058f534facc837")
VEC_SIG_HEX = ("6ab2cb4ccc3398f3f07f4e850b0a28d95d3083ed98d179016cd286e39a768852b8d423deccad0fa2"
               "e0ea9f78bcc826cc73c0a38d32622d114135e38ed5fb1c01")
VEC_PUB_HEX = "3deae44494d0794e183acf6c4445756458b27318cd531f355628249c54656e40"
VEC_LINES = ["multivoid-build-sig 1", "key 255", "target " + VEC_TARGET, "build 216",
             "sha256 " + VEC_SHA_HEX, "sig " + VEC_SIG_HEX]
VEC_TEXT = "\n".join(VEC_LINES) + "\n"


def drill_ed25519():
    for n, (sec, pub, msg, sig) in enumerate(RFC_VECTORS, start=1):
        seed, pub, msg, sig = (bytes.fromhex(v) for v in (sec, pub, msg, sig))
        tag = "ed25519 rfc test %d" % n
        check(tag + " public key", rs.ed25519_public(seed) == pub)
        check(tag + " signature", rs.ed25519_sign(seed, msg) == sig)
        check(tag + " verifies", rs.ed25519_verify(pub, msg, sig))
        check(tag + " refuses a flipped signature bit",
              not rs.ed25519_verify(pub, msg, flip_first_bit(sig)))
        if msg:
            check(tag + " refuses a flipped message bit",
                  not rs.ed25519_verify(pub, flip_first_bit(msg), sig))
        check(tag + " refuses a 63-byte signature", rs.ed25519_verify(pub, msg, sig[:63]) is False)
    sec, pub, msg, sig = (bytes.fromhex(v) for v in RFC_VECTORS[1])
    big_s = int.from_bytes(sig[32:], "little") + rs._L
    check("ed25519 refuses S plus the group order",
          not rs.ed25519_verify(pub, msg, sig[:32] + big_s.to_bytes(32, "little")))
    check("ed25519 refuses a 31-byte public key", rs.ed25519_verify(pub[:31], msg, sig) is False)
    check("ed25519 refuses a coordinate past the field prime",
          rs.ed25519_verify(b"\xff" * 32, msg, sig) is False)
    drill_noncanonical(msg, sec)


def drill_noncanonical(msg, seed):
    """Signatures that verify when a non-canonical point encoding is read as the identity.

    Each forgery is valid for the identity point, so a verifier that decodes the odd encoding
    accepts it: the refusal is then the decoder's canonical checks and nothing else."""
    identity = b"\x01" + bytes(31)
    y_past_p = b"\xee" + b"\xff" * 30 + b"\x7f"
    zero_x_odd = b"\x01" + bytes(30) + b"\x80"
    odd_encodings = (("y = p + 1", y_past_p), ("x = 0 with the sign bit", zero_x_odd))

    sig = rs._encode(rs._mul(5, rs._BASE)) + (5).to_bytes(32, "little")
    check("ed25519 control: the canonical identity key takes its forged signature",
          rs.ed25519_verify(identity, msg, sig) is True)
    for name, enc in odd_encodings:
        check("ed25519 refuses a public key with " + name,
              rs.ed25519_verify(enc, msg, sig) is False)

    pub = rs.ed25519_public(seed)
    secret = rs._expand(seed)[0]

    def forged_for_r(r_bytes):
        k = rs._digest_int(r_bytes, pub, msg) % rs._L
        return r_bytes + (k * secret % rs._L).to_bytes(32, "little")

    check("ed25519 control: the canonical identity R takes its forged signature",
          rs.ed25519_verify(pub, msg, forged_for_r(identity)) is True)
    for name, enc in odd_encodings:
        check("ed25519 refuses a signature R with " + name,
              rs.ed25519_verify(pub, msg, forged_for_r(enc)) is False)


def refused_grammar(text):
    try:
        rs.parse_sig(text)
    except rs.SignError as e:
        return e.step == "grammar" and e.code == 1
    return False


def drill_vector_and_grammar():
    sha = bytes.fromhex(VEC_SHA_HEX)
    check("vector public key", rs.ed25519_public(VEC_SEED).hex() == VEC_PUB_HEX)
    check("vector signed message",
          rs.signed_message(VEC_TARGET, VEC_BUILD, sha).hex() == VEC_MESSAGE_HEX)
    check("vector signature",
          rs.ed25519_sign(VEC_SEED, rs.signed_message(VEC_TARGET, VEC_BUILD, sha)).hex()
          == VEC_SIG_HEX)

    def with_line(index, line):
        lines = list(VEC_LINES)
        lines[index] = line
        return "\n".join(lines) + "\n"

    want = {"key_id": 255, "target": VEC_TARGET, "build": 216, "sha": sha,
            "sig": bytes.fromhex(VEC_SIG_HEX)}
    check("grammar accepts the vector", rs.parse_sig(VEC_TEXT) == want)
    check("grammar accepts CRLF ends", rs.parse_sig("\r\n".join(VEC_LINES) + "\r\n") == want)
    check("grammar accepts no final newline", rs.parse_sig("\n".join(VEC_LINES)) == want)
    check("grammar accepts the u32 ceiling", rs.parse_sig(with_line(3, "build 4294967295"))
          ["build"] == 4294967295)
    check("grammar accepts build 0", rs.parse_sig(with_line(3, "build 0"))["build"] == 0)
    check("grammar accepts a 23-character target",
          rs.parse_sig(with_line(2, "target " + "t" * 23))["target"] == "t" * 23)

    refusals = {
        "header 2": with_line(0, "multivoid-build-sig 2"),
        "header 10": with_line(0, "multivoid-build-sig 10"),
        "header with a suffix": with_line(0, "multivoid-build-sig 1 x"),
        "sig line missing": "\n".join(VEC_LINES[:5]) + "\n",
        "63-character sha256": with_line(4, "sha256 " + VEC_SHA_HEX[:-1]),
        "upper-case hex": with_line(4, "sha256 " + VEC_SHA_HEX.upper()),
        "upper-case signature hex": with_line(5, "sig " + VEC_SIG_HEX.upper()),
        "key 0": with_line(1, "key 0"),
        "key 256": with_line(1, "key 256"),
        "key with a leading zero": with_line(1, "key 07"),
        "key with a trailing Arabic-Indic digit": with_line(1, "key 1\u0665"),
        "build abc": with_line(3, "build abc"),
        "build 1_0": with_line(3, "build 1_0"),
        "build +5": with_line(3, "build +5"),
        "build with a trailing Arabic-Indic digit": with_line(3, "build 1\u0665"),
        "build with a leading zero": with_line(3, "build 007"),
        "build past the u32 ceiling": with_line(3, "build 4294967296"),
        "build with 11 digits": with_line(3, "build 12345678901"),
        "trailing whitespace": with_line(3, "build 216 "),
        "24-character target": with_line(2, "target " + "t" * 24),
        "target with a space": with_line(2, "target a b"),
        "empty target": with_line(2, "target "),
        "target with a DEL byte": with_line(2, "target a\x7fb"),
        "126-character signature": with_line(5, "sig " + VEC_SIG_HEX[:126]),
        "127-character signature":with_line(5, "sig " + VEC_SIG_HEX[:127]),
        "129-character signature": with_line(5, "sig " + VEC_SIG_HEX + "0"),
        "a seventh line": VEC_TEXT + "extra\n",
        "a blank last line": VEC_TEXT + "\n",
        "empty text": "",
        "lone CR ends": "\r".join(VEC_LINES) + "\r",
        "doubled CR ends": "\r\r\n".join(VEC_LINES) + "\r\r\n",
    }
    for name, text in refusals.items():
        check("grammar refuses " + name, refused_grammar(text))

    back = rs.parse_sig(rs.format_sig(7, "x.y", 99, sha, want["sig"]))
    check("format_sig then parse_sig",
          back == {"key_id": 7, "target": "x.y", "build": 99, "sha": sha, "sig": want["sig"]})


def drill_rows():
    text = rs.inc_path().read_bytes().decode("utf-8")
    try:
        rs.read_rows(text)
        parsed = True
    except rs.SignError:
        parsed = False
    check("rows: the tree's table parses", parsed)

    pub = bytes(range(32))
    fixture = bytes(range(64))
    row = rs.format_row(7, pub, fixture)
    head = "// a comment\n\n"
    expect = [(7, pub.hex(), fixture.hex())]
    check("rows: a printed row parses from LF", rs.read_rows(head + row + "\n") == expect)
    check("rows: a printed row parses from CRLF",
          rs.read_rows((head + row + "\n").replace("\n", "\r\n")) == expect)

    def refused_at(row_text, line_no):
        try:
            rs.read_rows(head + row_text + "\n")
        except rs.SignError as e:
            return (e.step == "table" and e.code == 2 and str(e)
                    == "release_sign: release_keys.inc line %d is not a row" % line_no)
        return False

    for bad in (0, 255, 1000):
        check("rows refuse id %d" % bad, refused_at(rs.format_row(bad, pub, fixture), 3))
    check("rows refuse id 010", refused_at(row.replace("(7,", "(010,"), 3))
    check("rows refuse an id with a trailing Arabic-Indic digit",
          refused_at(row.replace("(7,", "(1\u0665,"), 3))
    check("rows refuse a missing space", refused_at(row.replace(", ", ",", 1), 3))
    check("rows refuse an indented row", refused_at(" " + row, 3))
    try:
        rs.read_rows(row + "\n" + row + "\n")
        dup = False
    except rs.SignError as e:
        dup = (e.code == 2 and str(e) == "release_sign: release_keys.inc repeats id 7")
    check("rows refuse a repeated id", dup)


def call(fn, *args):
    buf = io.StringIO()
    try:
        rc = fn(*args, buf)
    except rs.SignError as e:
        buf.write(str(e) + "\n")
        rc = e.code
    return rc, buf.getvalue()


def sign_ns(dll, out, test_key=False, target="0.9.0n", build="216"):
    return argparse.Namespace(dll=str(dll), target=target, build=build, out=str(out),
                              test_key=test_key)


def verify_ns(dll, sig, trust=False):
    return argparse.Namespace(dll=str(dll), sig=str(sig), trust_test_key=trust)


def drill_test_key_path(tmp):
    dll = tmp / "tk.dll"
    sig = tmp / "tk.dll.sig"
    dll.write_bytes(b"test dll bytes " * 100)
    sha8 = rs.hashlib.sha256(dll.read_bytes()).hexdigest()[:8]
    sign_argv = ["sign", "--dll", str(dll), "--target", "0.9.0n", "--build", "216", "--out",
                 str(sig), "--test-key"]
    buf = io.StringIO()
    rc = rs.main(sign_argv, environ={}, out=buf)
    check("test-key: sign exits 0", rc == 0)
    check("test-key: sign says what it signed",
          buf.getvalue() == "release_sign: signed sha %s with key 255\n" % sha8)
    raw = sig.read_bytes()
    check("test-key: the file is LF-ended ASCII", raw.endswith(b"\n") and b"\r" not in raw)

    verify_argv = ["verify", "--dll", str(dll), "--sig", str(sig), "--trust-test-key"]
    buf = io.StringIO()
    rc = rs.main(verify_argv, environ={}, out=buf)
    check("test-key: verify exits 0", rc == 0)
    check("test-key: verify says the key", buf.getvalue() == "release_sign: verified key 255\n")

    for name, prefill in (("a valid file", None), ("a longer junk file", b"junk" * 500)):
        if prefill is not None:
            sig.write_bytes(prefill)
        rc = rs.main(sign_argv, environ={}, out=io.StringIO())
        check("test-key: signing over %s replaces it" % name, rc == 0 and sig.read_bytes() == raw)
        rc = rs.main(verify_argv, environ={}, out=io.StringIO())
        check("test-key: the replaced file verifies (%s)" % name, rc == 0)

    buf = io.StringIO()
    rc = rs.main(["verify", "--dll", str(dll), "--sig", str(sig)], environ={}, out=buf)
    check("test-key: not trusted without the switch",
          rc == 1 and buf.getvalue() == "release_sign: verify failed at key\n")

    no_rows = tmp / "tk_none.inc"
    one_row = tmp / "tk_one.inc"
    write_ascii(no_rows, "// no rows\n")
    write_ascii(one_row, rs.format_row(9, rs.ed25519_public(os.urandom(32)), bytes(64)) + "\n")
    for key_id, table_file, step in ((7, no_rows, "key"), (9, one_row, "signature")):
        relabelled = raw.decode("ascii").replace("key 255\n", "key %d\n" % key_id)
        sig.write_bytes(relabelled.encode("ascii"))
        with table_at(table_file):
            rc, out = call(rs.cmd_verify, verify_ns(dll, sig, True))
        check("test-key: a test-key .sig relabelled key %d fails at %s" % (key_id, step),
              rc == 1 and out == "release_sign: verify failed at %s\n" % step)
    sig.write_bytes(raw)

    dll.write_bytes(dll.read_bytes()[:-1] + b"!")
    buf = io.StringIO()
    rc = rs.main(verify_argv, environ={}, out=buf)
    check("test-key: a changed DLL byte fails at sha",
          rc == 1 and buf.getvalue() == "release_sign: verify failed at sha\n")
    dll.write_bytes(dll.read_bytes()[:-1] + b" ")

    text = raw.decode("ascii").split("\n")
    first = text[5][4]
    text[5] = text[5][:4] + ("0" if first != "0" else "1") + text[5][5:]
    sig.write_bytes("\n".join(text).encode("ascii"))
    buf = io.StringIO()
    rc = rs.main(verify_argv, environ={}, out=buf)
    check("test-key: a changed signature fails at signature",
          rc == 1 and buf.getvalue() == "release_sign: verify failed at signature\n")

    for name, data in (("lone CR", b"\r".join(l.encode() for l in VEC_LINES) + b"\r"),
                       ("non-ASCII", VEC_TEXT.encode() + b"\xff")):
        sig.write_bytes(data)
        rc, out = call(rs.cmd_verify, verify_ns(dll, sig, True))
        check("test-key: a %s .sig fails at grammar" % name,
              rc == 1 and out == "release_sign: verify failed at grammar\n")
    rc, out = call(rs.cmd_verify, verify_ns(tmp / "none.dll", sig, True))
    check("test-key: a missing DLL fails at read",
          rc == 1 and out == "release_sign: verify failed at read\n")

    for label, ns in (("target", sign_ns(dll, sig, True, target="a b")),
                      ("build", sign_ns(dll, sig, True, build="4294967296")),
                      ("build text", sign_ns(dll, sig, True, build="1_0")),
                      ("build zero-led", sign_ns(dll, sig, True, build="007")),
                      ("target with a DEL byte", sign_ns(dll, sig, True, target="a\x7fb"))):
        rc, out = call(rs.cmd_sign, ns, {})
        check("test-key: sign refuses a bad " + label, rc == 2 and "--" + label.split()[0] in out)
    rc, out = call(rs.cmd_sign, sign_ns(tmp / "none.dll", sig, True), {})
    check("test-key: sign names an unreadable DLL",
          rc == 2 and out == "release_sign: cannot read --dll\n")
    rc, out = call(rs.cmd_sign, sign_ns(dll, tmp / "no" / "dir" / "x.sig", True), {})
    check("test-key: sign names an unwritable output",
          rc == 2 and out == "release_sign: cannot write --out\n")


def drill_release_path(tmp):
    seed = os.urandom(32)
    pub = rs.ed25519_public(seed)
    fixture = rs.ed25519_sign(seed, rs.signed_message("selftest-fixture", 0, bytes(32)))
    table = tmp / "keys.inc"
    comment = "// a table of one row\n"
    dll = tmp / "rel.dll"
    sig = tmp / "rel.dll.sig"
    dll.write_bytes(b"release dll bytes " * 64)
    sha8 = rs.hashlib.sha256(dll.read_bytes()).hexdigest()[:8]
    env = {rs.SEED_ENV: seed.hex()}
    original = rs.inc_path
    try:
        rs.inc_path = lambda: table
        write_ascii(table, comment + rs.format_row(9, pub, fixture) + "\n")
        rc, out = call(rs.cmd_sign, sign_ns(dll, sig), env)
        check("release: sign with the table's key",
              rc == 0 and out == "release_sign: signed sha %s with key 9\n" % sha8)
        rc, out = call(rs.cmd_verify, verify_ns(dll, sig))
        check("release: verify without the test-key switch",
              rc == 0 and out == "release_sign: verified key 9\n")

        write_ascii(table, comment)
        rc, out = call(rs.cmd_sign, sign_ns(dll, sig), env)
        check("release: an empty table refuses sign",
              rc == 2 and out == "release_sign: the key is not in release_keys.inc\n")
        rc, out = call(rs.cmd_verify, verify_ns(dll, sig))
        check("release: an empty table refuses verify at key",
              rc == 1 and out == "release_sign: verify failed at key\n")

        table.unlink()
        rc, out = call(rs.cmd_sign, sign_ns(dll, sig), env)
        check("release: no table file refuses sign",
              rc == 2 and out == "release_sign: release_keys.inc not found at the checkout\n")
        rc, out = call(rs.cmd_verify, verify_ns(dll, sig))
        check("release: no table file refuses verify",
              rc == 2 and out == "release_sign: release_keys.inc not found at the checkout\n")

        write_ascii(table, comment + "not a row\n")
        rc, out = call(rs.cmd_verify, verify_ns(dll, sig))
        check("release: a malformed table is exit 2 inside verify", rc == 2)
    finally:
        rs.inc_path = original
    drill_release_two_rows(tmp, dll, sig, sha8)


def drill_release_two_rows(tmp, dll, sig, sha8):
    """A table of two rows: the row is chosen by the key, never by position."""
    seeds = {9: os.urandom(32), 10: os.urandom(32)}
    stranger = os.urandom(32)
    fixture_msg = rs.signed_message("selftest-fixture", 0, bytes(32))
    table = tmp / "two_rows.inc"
    write_ascii(table, "".join(
        rs.format_row(i, rs.ed25519_public(sd), rs.ed25519_sign(sd, fixture_msg)) + "\n"
        for i, sd in seeds.items()))
    with table_at(table):
        for key_id, sd in seeds.items():
            rc, out = call(rs.cmd_sign, sign_ns(dll, sig), {rs.SEED_ENV: sd.hex()})
            check("release two rows: the seed of row %d signs under id %d" % (key_id, key_id),
                  rc == 0 and out == "release_sign: signed sha %s with key %d\n" % (sha8, key_id))
            rc, out = call(rs.cmd_verify, verify_ns(dll, sig))
            check("release two rows: its file verifies under id %d" % key_id,
                  rc == 0 and out == "release_sign: verified key %d\n" % key_id)
        rc, out = call(rs.cmd_sign, sign_ns(dll, sig), {rs.SEED_ENV: stranger.hex()})
        check("release two rows: a seed in no row is refused by sign",
              rc == 2 and out == "release_sign: the key is not in release_keys.inc\n")
        sha = rs.hashlib.sha256(dll.read_bytes()).digest()
        wrong = rs.ed25519_sign(seeds[9], rs.signed_message("0.9.0n", 216, sha))
        write_ascii(sig, rs.format_sig(10, "0.9.0n", 216, sha, wrong))
        rc, out = call(rs.cmd_verify, verify_ns(dll, sig))
        check("release two rows: row 9's signature under id 10 fails at signature",
              rc == 1 and out == "release_sign: verify failed at signature\n")


def drill_seed():
    good = os.urandom(32).hex()
    cases = (("absent", {}, "is missing"),
             ("63 characters", {rs.SEED_ENV: good[:63]}, "is not 64 lowercase hex characters"),
             ("upper case", {rs.SEED_ENV: good.upper()}, "is not 64 lowercase hex characters"),
             ("trailing newline", {rs.SEED_ENV: good + "\n"},
              "is not 64 lowercase hex characters"),
             ("empty", {rs.SEED_ENV: ""}, "is not 64 lowercase hex characters"))
    for name, env, tail in cases:
        rc, out = call(rs.cmd_sign, sign_ns("x", "y"), env)
        check("seed: %s exits 3 with the dictated message" % name,
              rc == 3 and out == "release_sign: %s %s\n" % (rs.SEED_ENV, tail)
              and good not in out)


def leaks(seed, text):
    hexa = seed.hex()
    forms = [hexa, hexa.upper(), repr(seed), base64.b64encode(seed).decode().rstrip("="),
             base64.urlsafe_b64encode(seed).decode().rstrip("=")]
    return [f for f in forms if f in text]


def drill_process(tmp):
    tree = tmp / "mirror"
    script = tree / ".github" / "ci" / "release_sign.py"
    inc = tree / "src" / "votv-coop" / "include" / "coop" / "build_trust" / "release_keys.inc"
    script.parent.mkdir(parents=True)
    inc.parent.mkdir(parents=True)
    shutil.copyfile(HERE / "release_sign.py", script)
    write_ascii(inc, "// an empty table\n")
    dll = tmp / "proc.dll"
    sig = tmp / "proc.sig"
    dll.write_bytes(b"process dll bytes " * 32)
    seed = os.urandom(32)
    pub = rs.ed25519_public(seed)
    fixture = rs.ed25519_sign(seed, rs.signed_message("selftest-fixture", 0, bytes(32)))

    def run(seed_value):
        env = dict(os.environ)
        env.pop(rs.SEED_ENV, None)
        if seed_value is not None:
            env[rs.SEED_ENV] = seed_value
        argv = [sys.executable, "-I", "-B", str(script), "sign", "--dll", str(dll), "--target",
                "0.9.0n", "--build", "216", "--out", str(sig)]
        p = subprocess.run(argv, env=env, capture_output=True, text=True, timeout=120)
        return p.returncode, p.stdout.replace("\r\n", "\n"), p.stdout + p.stderr

    rc, out, both = run("zz")
    check("process: a malformed seed exits 3",
          rc == 3 and out == "release_sign: %s is not 64 lowercase hex characters\n" % rs.SEED_ENV)
    rc, out, both = run(seed.hex())
    check("process: a valid seed with an empty table exits 2 at the key",
          rc == 2 and out == "release_sign: the key is not in release_keys.inc\n")
    check("process: the seed is nowhere in that output", not leaks(seed, both))
    write_ascii(inc, "// one row\n" + rs.format_row(4, pub, fixture) + "\n")
    rc, out, both = run(seed.hex())
    check("process: the seed with its row exits 0 with key 4",
          rc == 0 and out.endswith("with key 4\n"))
    check("process: the seed is nowhere in that output or the written file",
          not leaks(seed, both + sig.read_text(encoding="ascii")))
    check("process: the signature file verifies under the row",
          rs.ed25519_verify(pub, rs.signed_message("0.9.0n", 216, rs.hashlib.sha256(
              dll.read_bytes()).digest()), rs.parse_sig(sig.read_text(encoding="ascii"))["sig"]))


def guarded(group, *args):
    """A group that dies on an exception it did not expect is a FAIL line, not a traceback."""
    try:
        group(*args)
    except Exception as e:
        check("%s raised %s" % (group.__name__, type(e).__name__), False)


def main():
    with tempfile.TemporaryDirectory() as t:
        tmp = pathlib.Path(t)
        guarded(drill_ed25519)
        guarded(drill_vector_and_grammar)
        guarded(drill_rows)
        guarded(drill_test_key_path, tmp)
        guarded(drill_release_path, tmp)
        guarded(drill_seed)
        guarded(drill_process, tmp)
    if all(RESULTS):
        print("release_sign drill: ALL PASS (%d checks)" % len(RESULTS))
        return 0
    print("release_sign drill: %d of %d checks FAILED" % (RESULTS.count(False), len(RESULTS)))
    return 1


if __name__ == "__main__":
    sys.exit(main())
