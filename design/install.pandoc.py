#!/usr/bin/env python3
"""Install pandoc and typst (the PDF engine) for design/assemble.py.

Both are downloaded as static binaries from their official GitHub releases
into ~/.local/bin, so no sudo is required. Re-running upgrades to the latest
releases.

Usage: python3 design/install.pandoc.py [--prefix DIR]
"""

import argparse
import io
import json
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request

PANDOC_API = "https://api.github.com/repos/jgm/pandoc/releases/latest"
TYPST_URL = "https://github.com/typst/typst/releases/latest/download/typst-{arch}-unknown-linux-musl.tar.xz"


def fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent": "goober-install-pandoc"})
    with urllib.request.urlopen(request) as response:
        return response.read()


def extract_binary(archive_bytes, name, destination):
    """Pull the single file called `name` out of a tarball into destination."""
    with tarfile.open(fileobj=io.BytesIO(archive_bytes)) as archive:
        for member in archive.getmembers():
            if member.isfile() and os.path.basename(member.name) == name:
                source = archive.extractfile(member)
                target = os.path.join(destination, name)
                with open(target + ".tmp", "wb") as out:
                    shutil.copyfileobj(source, out)
                os.chmod(target + ".tmp", 0o755)
                os.replace(target + ".tmp", target)
                return target
    raise RuntimeError(f"{name} not found in archive")


def install_pandoc(prefix):
    arch = {"x86_64": "amd64", "aarch64": "arm64"}[platform.machine()]
    release = json.loads(fetch(PANDOC_API))
    suffix = f"-linux-{arch}.tar.gz"
    asset = next(a for a in release["assets"] if a["name"].endswith(suffix))
    print(f"pandoc: downloading {asset['name']}")
    return extract_binary(fetch(asset["browser_download_url"]), "pandoc", prefix)


def install_typst(prefix):
    arch = {"x86_64": "x86_64", "aarch64": "aarch64"}[platform.machine()]
    url = TYPST_URL.format(arch=arch)
    print(f"typst: downloading {url.rsplit('/', 1)[1]}")
    return extract_binary(fetch(url), "typst", prefix)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--prefix", default=os.path.expanduser("~/.local/bin"),
                        help="directory to install binaries into (default: ~/.local/bin)")
    args = parser.parse_args()

    if platform.system() != "Linux":
        sys.exit("This installer only supports Linux.")
    os.makedirs(args.prefix, exist_ok=True)

    for installed in (install_pandoc(args.prefix), install_typst(args.prefix)):
        version = subprocess.run([installed, "--version"], capture_output=True, text=True).stdout.splitlines()[0]
        print(f"  installed {installed} ({version})")

    if args.prefix not in os.environ.get("PATH", "").split(os.pathsep):
        print(f"\nNote: {args.prefix} is not on your PATH; assemble.py will still find it there.")
    print("\nDone. Build the book with: python3 design/assemble.py")


if __name__ == "__main__":
    main()
