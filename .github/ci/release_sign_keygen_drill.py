#!/usr/bin/env python3
"""release_sign_keygen_drill -- proves `release_sign.py keygen` keeps the key's custody.

Standard library only. A fake runner answers canned `gh` output and records every call's argv and
keyword arguments, so no process starts and the real `gh` is never run; each run happens inside a
temporary working directory with stdout and stderr captured. Two mutants are always run and the
same assertion helper as the green arms must flag each of them.

Run: python -I -B .github/ci/release_sign_keygen_drill.py
"""
import argparse
import contextlib
import importlib.util
import io
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent

_spec = importlib.util.spec_from_file_location("release_sign_drill_lib",
                                               HERE / "release_sign_drill_lib.py")
lib = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(lib)

rs = lib.rs
check = lib.check
guarded = lib.guarded
table_at = lib.table_at
write_ascii = lib.write_ascii
leaks = lib.leaks


SECRET_ARGV = ["gh", "secret", "set", rs.SEED_ENV, "--env", "release", "--repo",
               "VOTV-MP/Multivoid"]
UNPROTECTED = "release_sign: the release environment is not protected as the runbook sets it (%s)"


def canned_env(rule=None, **over):
    """GitHub's REST answer for a protected environment; `rule` edits the reviewers rule."""
    reviewers = {"id": 2, "node_id": "n2", "type": "required_reviewers",
                 "prevent_self_review": False,
                 "reviewers": [{"type": "Team", "reviewer": {"name": "maintainers", "id": 7}}]}
    reviewers.update(rule or {})
    env = {"name": "release", "can_admins_bypass": False,
           "protection_rules": [{"id": 1, "node_id": "n1", "type": "branch_policy"}, reviewers],
           "deployment_branch_policy": {"protected_branches": False,
                                        "custom_branch_policies": True}}
    env.update(over)
    return env


def canned_policies(*extra):
    items = [("v*", "tag"), ("main", "branch")] + list(extra)
    return {"total_count": len(items), "branch_policies": [
        {"id": 3 + i, "node_id": "n%d" % i, "name": n, "type": t} for i, (n, t) in enumerate(items)]}


class FakeGh:
    """A runner that answers from canned values and records every call; it starts no process."""

    def __init__(self, environment=None, policies=None, secret=lambda text: (0, "", "")):
        self.environment = canned_env() if environment is None else environment
        self.policies = canned_policies() if policies is None else policies
        self.secret = secret
        self.calls = []

    def __call__(self, argv, **kwargs):
        self.calls.append((list(argv), dict(kwargs)))
        if argv[:3] == ["gh", "secret", "set"]:
            answer = self.secret(kwargs.get("input"))
        else:
            answer = self.policies if argv[2].endswith("/deployment-branch-policies") \
                else self.environment
            if isinstance(answer, (dict, list)):
                answer = (0, json.dumps(answer), "")
        if isinstance(answer, Exception):
            raise answer
        if kwargs.get("check") and answer[0] != 0:
            raise subprocess.CalledProcessError(answer[0], argv, answer[1], answer[2])
        return subprocess.CompletedProcess(argv, answer[0], answer[1], answer[2])

    def secret_calls(self):
        return [c for c in self.calls if c[0][:3] == ["gh", "secret", "set"]]


def drive(keygen, fake, table, key_id=2, repo="VOTV-MP/Multivoid"):
    """Runs a keygen inside a temporary working directory with stdout and stderr captured."""
    out, cap_out, cap_err = io.StringIO(), io.StringIO(), io.StringIO()
    args = argparse.Namespace(id=key_id, repo=repo)
    here = os.getcwd()
    with tempfile.TemporaryDirectory() as work:
        os.chdir(work)
        try:
            with table_at(table), contextlib.redirect_stdout(cap_out), \
                    contextlib.redirect_stderr(cap_err):
                try:
                    rc = keygen(args, fake, out)
                except rs.SignError as e:
                    out.write(str(e) + "\n")
                    rc = e.code
        finally:
            os.chdir(here)
        left = os.listdir(work)
    return {"rc": rc, "out": out.getvalue() + cap_out.getvalue(), "stderr": cap_err.getvalue(),
            "left": left}


def keygen_flaws(keygen, fake, table):
    """Every custody rule a green keygen run must keep; an empty list is a clean run."""
    table_before = table.read_bytes()
    r = drive(keygen, fake, table)
    flaws = []

    def need(name, ok):
        if not ok:
            flaws.append(name)

    calls, secrets = fake.calls, fake.secret_calls()
    need("exit 0", r["rc"] == 0)
    need("stderr empty", r["stderr"] == "")
    need("three calls, the secret last",
         len(calls) == 3 and len(secrets) == 1 and calls[2] is secrets[0]
         and all(c[0][:2] == ["gh", "api"] for c in calls[:2]))
    need("the dictated gh secret set argv", bool(secrets) and secrets[0][0] == SECRET_ARGV)
    seed_hex = secrets[0][1].get("input") if secrets else None
    need("the input is 64 lowercase hex",
         isinstance(seed_hex, str) and re.fullmatch(r"[0-9a-f]{64}", seed_hex) is not None)
    if flaws and not isinstance(seed_hex, str):
        return flaws
    seed = bytes.fromhex(seed_hex) if re.fullmatch(r"[0-9a-f]{64}", seed_hex or "") else b"\0" * 32
    pub = rs.ed25519_public(seed)
    try:
        rows = rs.read_rows(r["out"])
    except rs.SignError:
        rows = []
    need("exactly the row and a newline", len(rows) == 1 and rows[0][0] == 2
         and rows[0][1] == pub.hex() and r["out"] == rs.format_row(
             2, pub, bytes.fromhex(rows[0][2])) + "\n")
    need("the fixture verifies under the row's key", len(rows) == 1 and rs.ed25519_verify(
        pub, rs.signed_message("selftest-fixture", 0, bytes(32)), bytes.fromhex(rows[0][2])))
    elsewhere = repr([(argv, {k: v for k, v in kw.items() if k != "input"}) for argv, kw in calls])
    need("the seed only in the input", not leaks(seed, elsewhere + r["out"] + r["stderr"]))
    need("check is never passed", all("check" not in kw for _, kw in calls))
    need("the table is unchanged", table.read_bytes() == table_before)
    need("the working directory is empty", r["left"] == [])
    return flaws


def leaking_keygen(args, runner, out):
    seen = []

    def spy(argv, **kwargs):
        seen.append(kwargs.get("input"))
        return runner(argv, **kwargs)

    rc = rs.cmd_keygen(args, spy, out)
    out.write(seen[-1] + "\n")
    return rc


def unchecked_keygen(args, runner, out):
    runner(SECRET_ARGV, input=os.urandom(32).hex(), capture_output=True, text=True,
           encoding="utf-8")
    return rs.cmd_keygen(args, runner, out)


def drill_keygen(tmp):
    table = tmp / "kg.inc"
    write_ascii(table, "// one row\n" + rs.format_row(1, rs.ed25519_public(os.urandom(32)),
                                                       bytes(64)) + "\n")
    user = {"reviewers": [{"type": "User", "reviewer": {"login": "x", "id": 1}}]}
    for name, fake in (("a Team reviewer", FakeGh()),
                       ("a User reviewer", FakeGh(canned_env(user)))):
        flaws = keygen_flaws(rs.cmd_keygen, fake, table)
        check("keygen green with " + name + (": " + ", ".join(flaws) if flaws else ""), not flaws)
    for name, mutant in (("a keygen that prints the seed", leaking_keygen),
                         ("a keygen that skips the protection check", unchecked_keygen)):
        check("mutant: %s caught" % name, bool(keygen_flaws(mutant, FakeGh(), table)))

    refusals = {
        "no reviewer": (canned_env({"reviewers": []}), None, "no reviewer"),
        "no reviewers rule": (canned_env({"type": "wait_timer"}), None, "required reviewers"),
        "prevent_self_review true": (canned_env({"prevent_self_review": True}), None,
                                              "self review"),
        "admins can bypass": (canned_env(can_admins_bypass=True), None, "admins can bypass"),
        "admin bypass absent": ({k: v for k, v in canned_env().items() if k != "can_admins_bypass"},
                                None, "admins can bypass"),
        "protected_branches true": (canned_env(deployment_branch_policy={
            "protected_branches": True, "custom_branch_policies": True}), None,
            "deployment branch policy"),
        "custom_branch_policies false": (canned_env(deployment_branch_policy={
            "protected_branches": False, "custom_branch_policies": False}), None,
            "deployment branch policy"),
        "a third policy": (None, canned_policies(("dev", "branch")), "branch policies"),
        "a missing policy": (None, {"total_count": 1, "branch_policies": [
            {"id": 3, "node_id": "n3", "name": "v*", "type": "tag"}]}, "branch policies"),
        "a policy of the wrong type": (None, {"total_count": 2, "branch_policies": [
            {"id": 3, "node_id": "n3", "name": "v*", "type": "branch"},
            {"id": 4, "node_id": "n4", "name": "main", "type": "branch"}]}, "branch policies"),
        "an environment that is not an object": ([1, 2], None, "admins can bypass"),
    }
    gh_failed = {
        "the environment missing": (FakeGh(environment=(1, "", "gh: Not Found (HTTP 404)\nmore")),
                                    "release_sign: gh api failed: gh: Not Found (HTTP 404)"),
        "gh missing": (FakeGh(environment=FileNotFoundError("gh")),
                       "release_sign: gh is not installed or cannot run"),
        "a non-JSON answer": (FakeGh(policies=(0, "<html>", "")),
                              "release_sign: gh api failed (the answer is not JSON)"),
    }
    cases = [(name, FakeGh(env, pol), UNPROTECTED % check_name)
             for name, (env, pol, check_name) in refusals.items()]
    cases += [(name, fake, text) for name, (fake, text) in gh_failed.items()]
    for name, fake, text in cases:
        r = drive(rs.cmd_keygen, fake, table)
        check("keygen refuses " + name,
              r["rc"] == 3 and r["out"] == text + "\n" and r["stderr"] == ""
              and not fake.secret_calls() and r["left"] == [])

    fake = FakeGh(secret=lambda text: (1, text, "rejected " + text))
    r = drive(rs.cmd_keygen, fake, table)
    check("keygen secret fails: the message is dictated and holds nothing else",
          r["rc"] == 3 and r["out"] == "release_sign: gh secret set failed (exit 1)\n"
          and r["stderr"] == "" and len(fake.calls) == 3)
    fake = FakeGh(secret=lambda text: FileNotFoundError("gh"))
    r = drive(rs.cmd_keygen, fake, table)
    check("keygen secret set cannot run: exit 3",
          r["rc"] == 3 and r["out"] == "release_sign: gh is not installed or cannot run\n")

    write_ascii(table, "// two rows\n" + "".join(
        rs.format_row(i, rs.ed25519_public(os.urandom(32)), bytes(64)) + "\n" for i in (3, 5)))
    range_text, rank_text = "--id must be 1..254", "--id must be greater than 5"
    absent = "release_keys.inc not found at the checkout"
    for label, where, key_id, repo, text in (
            ("--id 0", table, 0, "VOTV-MP/Multivoid", range_text),
            ("--id 255", table, 255, "VOTV-MP/Multivoid", range_text),
            ("--id 5, the highest row", table, 5, "VOTV-MP/Multivoid", rank_text),
            ("--id 4, below it", table, 4, "VOTV-MP/Multivoid", rank_text),
            ("a bad --repo", table, 6, "not a repo", "--repo must be owner/name"),
            ("no table", tmp / "absent.inc", 6, "VOTV-MP/Multivoid", absent)):
        fake = FakeGh()
        r = drive(rs.cmd_keygen, fake, where, key_id=key_id, repo=repo)
        check("keygen ids: %s is exit 2 with no runner call" % label,
              r["rc"] == 2 and r["out"] == "release_sign: " + text + "\n" and not fake.calls)


def main():
    with tempfile.TemporaryDirectory() as t:
        guarded(drill_keygen, pathlib.Path(t))
    return lib.finish("release_sign keygen drill")


if __name__ == "__main__":
    sys.exit(main())
