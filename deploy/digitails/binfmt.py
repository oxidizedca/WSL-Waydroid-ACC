"""Inspect/switch only the two known Android ARM64 rules; never flush binfmt_misc."""
import sys
from pathlib import Path

NDK = "/system/bin/ndk_translation_program_runner_binfmt_misc_arm64"
DIG = "/system/bin/berberis_program_runner_binfmt_misc_arm64"
RUNNERS = {"ndk": NDK, "digitalis": DIG}
NAMES = ("arm64_dyn", "arm64_exe")


def rules(bundle):
    result = {}
    for name in NAMES:
        spec = (bundle / name).read_text().strip()
        _, label, kind, offset, magic, mask, runner, flags = spec.split(":")
        if (label, kind, offset, mask, runner, flags) != (name, "M", "", "", DIG, "P"):
            raise ValueError(f"Unexpected bundled rule: {name}")
        result[name] = (spec, magic.encode().decode("unicode_escape").encode("latin1").hex())
    return result


def state(fs, name, magic):
    p = fs / name
    if not p.exists():
        return "absent"
    lines = p.read_text().splitlines()
    for key, runner in RUNNERS.items():
        if lines == ["enabled", "interpreter " + runner, "flags: P", "offset 0", "magic " + magic]:
            return key
    raise ValueError(f"Unrecognized or disabled binfmt rule: {name}; refusing to overwrite")


def main():
    fs, bundle = map(Path, sys.argv[1:3])
    if not (fs / "register").exists() or (fs / "status").read_text().strip() != "enabled":
        raise ValueError("binfmt_misc unavailable or disabled in the Waydroid namespace")
    specs = rules(bundle)
    previous = [state(fs, n, specs[n][1]) for n in NAMES]
    targets = sys.argv[3:]
    if targets:
        if len(targets) != 2 or any(t not in (*RUNNERS, "absent") for t in targets):
            raise ValueError("Invalid handler selection")
        for n, old, new in zip(NAMES, previous, targets):
            if old == new:
                continue
            spec, magic = specs[n]
            if old != "absent":
                (fs / n).write_text("-1")
            try:
                if new != "absent":
                    (fs / "register").write_bytes(spec.replace(DIG, RUNNERS[new]).encode())
            except OSError:
                if old != "absent" and not (fs / n).exists():
                    (fs / "register").write_bytes(spec.replace(DIG, RUNNERS[old]).encode())
                raise
            if state(fs, n, magic) != new:
                raise RuntimeError(f"Rule verification failed: {n}")
    print(" ".join(state(fs, n, specs[n][1]) for n in NAMES))


if __name__ == "__main__":
    main()
