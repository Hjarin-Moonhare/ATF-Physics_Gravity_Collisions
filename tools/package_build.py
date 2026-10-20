"""Packs a test build for a tester: dist\\TFPhysics_<name>.zip = SFSE/Plugins/TFPhysics.dll (build\\TFPhysics.dll, the
logging build from build.bat) + SFSE/Plugins/TFPhysics.ini (config\\TFPhysics.ini). Never overwrites a package.
Usage: python tools\\package_build.py <name, e.g. test1>
"""
import hashlib
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main(name):
    out = os.path.join(ROOT, "dist", f"TFPhysics_{name}.zip")
    if os.path.exists(out):
        sys.exit(f"{out} exists already: packages are never overwritten")
    dll = os.path.join(ROOT, "build", "TFPhysics.dll")
    ini = os.path.join(ROOT, "config", "TFPhysics.ini")
    if not os.path.exists(dll):
        sys.exit(f"{dll} not found: run build.bat first")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("SFSE/", b"")
        z.writestr("SFSE/Plugins/", b"")
        z.write(dll, "SFSE/Plugins/TFPhysics.dll")
        z.write(ini, "SFSE/Plugins/TFPhysics.ini")
    for p in (dll, ini, out):
        data = open(p, "rb").read()
        print(f"{p}: {len(data):,} bytes, sha256 {hashlib.sha256(data).hexdigest()[:16]}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
