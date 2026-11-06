#!/usr/bin/env python3
"""Make the sysroot a Linux cross build compiles against (tools/build.py --arch).

    tools/linux_sysroot.py --arch arm64 [--out DIR]

A Debian 12 (bookworm) container for the target processor installs the
headers and libraries the app links or loads: glibc and libstdc++, SDL's X11,
Wayland and audio development packages (the list checks.yml installs on the
CI's Linux runner), and Mesa's Vulkan drivers, so a test binary can run under
qemu-user against lavapipe. Its /usr and /etc are copied out; absolute
symbolic links are made relative, so the linker and qemu -L resolve them
inside the sysroot. An app built against it runs on Debian 12 (ChromeOS's
Linux environment, Raspberry Pi OS) and anything newer, Fedora Asahi Remix
included; HommaRe needs glibc 2.35 and GCC 12's libstdc++, so Ubuntu 22.04 too.

Needs podman or docker, rootless is enough, and the kernel's binfmt_misc
entry for the processor (Fedora: qemu-user-static-aarch64; Debian/Ubuntu:
qemu-user-static) so the container's apt runs under qemu. Nothing needs root.
The default output is the kit's build/sysroots/debian12-<arch>, where
tools/build.py --arch looks for it.
"""

import argparse
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
RELEASE = "bookworm"
ARCHES = {"arm64": {"platform": "linux/arm64", "triple": "aarch64-linux-gnu"}}
PACKAGES = [
    "libc6-dev", "libstdc++-12-dev", "libgcc-12-dev",
    # SDL3's Linux build dependencies (README-linux; .github/workflows/checks.yml).
    "libasound2-dev", "libpulse-dev", "libaudio-dev", "libjack-jackd2-dev", "libsndio-dev",
    "libx11-dev", "libxext-dev", "libxrandr-dev", "libxcursor-dev", "libxfixes-dev", "libxi-dev",
    "libxss-dev", "libxtst-dev", "libxkbcommon-dev", "libdrm-dev", "libgbm-dev", "libgl1-mesa-dev",
    "libgles2-mesa-dev", "libegl1-mesa-dev", "libdbus-1-dev", "libibus-1.0-dev", "libudev-dev",
    "libpipewire-0.3-dev", "libwayland-dev", "libdecor-0-dev", "liburing-dev",
    # Not linked: the Vulkan loader and Mesa's drivers, for runs under qemu-user.
    "libvulkan1", "mesa-vulkan-drivers",
]
# What a sysroot needs from the image; the rest (dev, proc, var, ...) stays out.
KEEP = ("usr", "etc", "lib", "lib64", "bin", "sbin")


def container_engine(name=None):
    for engine in ([name] if name else ["podman", "docker"]):
        if shutil.which(engine):
            return engine
    sys.exit("Needs podman or docker")


def relative_link(member):
    """An absolute symlink target as a path relative to the link's own folder."""
    target = PurePosixPath(member.linkname)
    if not target.is_absolute():
        return member.linkname
    here = PurePosixPath("/" + member.name).parent
    return os.path.relpath(target, here)


def unpack(stream, out):
    """Extract the exported filesystem's KEEP folders: regular files, folders and links only."""
    with tarfile.open(fileobj=stream, mode="r|") as tar:
        for member in tar:
            name = member.name.lstrip("./")
            if not name or name.split("/")[0] not in KEEP:
                continue
            if not (member.isfile() or member.isdir() or member.issym() or member.islnk()):
                continue
            member.name = name
            if member.issym():
                member.linkname = relative_link(member)
            elif member.islnk():
                member.linkname = member.linkname.lstrip("./")
            tar.extract(member, out, filter="data")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--arch", choices=sorted(ARCHES), default="arm64")
    parser.add_argument("--out", type=Path, help="default: kit/build/sysroots/debian12-<arch>")
    parser.add_argument("--engine", choices=("podman", "docker"), help="default: podman, else docker")
    args = parser.parse_args()
    arch = ARCHES[args.arch]
    out = (args.out or ROOT / "build/sysroots" / ("debian12-" + args.arch)).resolve()
    engine = container_engine(args.engine)
    name = "recomp-sysroot-%s-%d" % (args.arch, os.getpid())
    script = ("export DEBIAN_FRONTEND=noninteractive && apt-get update -qq && "
              "apt-get install -y -qq --no-install-recommends " + " ".join(PACKAGES) + " >/dev/null")
    print("Installing %d packages in a %s %s container (apt runs under qemu: a few minutes)"
          % (len(PACKAGES), RELEASE, arch["platform"]), flush=True)
    subprocess.run([engine, "run", "--name", name, "--platform", arch["platform"],
                    "docker.io/library/debian:" + RELEASE, "sh", "-c", script], check=True)
    try:
        if out.exists():
            shutil.rmtree(out)
        out.mkdir(parents=True)
        export = subprocess.Popen([engine, "export", name], stdout=subprocess.PIPE)
        unpack(export.stdout, out)
        if export.wait() != 0:
            sys.exit("%s export failed" % engine)
    finally:
        subprocess.run([engine, "rm", "-f", name], stdout=subprocess.DEVNULL, check=False)
    if not (out / "usr/include" / arch["triple"]).is_dir():
        sys.exit("No usr/include/%s in %s: the install failed" % (arch["triple"], out))
    (out / "recomp-sysroot.txt").write_text("debian:%s %s\n%s\n" % (RELEASE, arch["platform"], "\n".join(PACKAGES)))
    print("Sysroot in %s\n  tools/build.py --arch %s uses it from there, or with --sysroot / "
          "RECOMP_LINUX_SYSROOT" % (out, args.arch))


if __name__ == "__main__":
    main()
