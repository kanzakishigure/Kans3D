#!/usr/bin/env python3
"""Checks and installs the Kans3D build dependencies, then configures CMake.

    python script/Setup.py                    interactive menu in a terminal (Tab/arrows/mouse, Python 3.10+)
    python script/Setup.py --non-interactive  check, ask once, install everything missing, configure
    python script/Setup.py --check            report only; exit code 1 if anything required is missing
    python script/Setup.py --yes              install everything missing without asking, configure
    python script/Setup.py --build            also build the editor (Debug)
    python script/Setup.py --tests            include test dependencies (GoogleTest, software Vulkan)

Linux: Arch (pacman), Debian/Ubuntu (apt), Fedora (dnf). Windows: winget.
The menu uses prompt_toolkit from the pure-Python wheels in script/third_party (no pip, no network);
everything else is the standard library, so it runs on a fresh machine.
"""
import argparse
import ctypes
import hashlib
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
THIRD_PARTY = Path(__file__).resolve().parent / "third_party"
REQUIRED_CMAKE = (3, 21)
REQUIRED_DOTNET_MAJOR = 10
FFMPEG_DIR = ROOT / "Kans3D" / "vendor" / "ffmpeg-bin"
FFMPEG_ARCHIVE = "ffmpeg-n8.1.3-39-g660b5aa7db-win64-lgpl-shared-8.1"
FFMPEG_URL = (
    "https://github.com/FFmpeg-Builds/FFmpeg-Builds/releases/download/autobuild-2026-11-22-12-36/"
    + FFMPEG_ARCHIVE + ".zip"
)
FFMPEG_SHA256 = "fd6d485f1ed515b1ca30c99f934dbb4a9c18de2949d5f5c2a03b069a8420ce38"
FFMPEG_LIBRARIES = ("avformat", "avcodec", "avutil")
STEAM_AUDIO_DIR = ROOT / "Kans3D" / "vendor" / "steamaudio-bin"
STEAM_AUDIO_VERSION = "4.8.1"
STEAM_AUDIO_URL = (
    f"https://github.com/ValveSoftware/steam-audio/releases/download/v{STEAM_AUDIO_VERSION}/"
    f"steamaudio_{STEAM_AUDIO_VERSION}.zip"
)
STEAM_AUDIO_SHA256 = "4a0aa5ec1176f38f0b0993a37c2259d9e86f27e22d5e24f83ec4c3cb9a1d5449"
TEST_DIR = ROOT / "test"
VENDORED_GTEST = TEST_DIR / "vendor" / "googletest"


def run(command, **kwargs):
    """Returns (exit code, combined output); a missing executable is exit code 127."""
    try:
        result = subprocess.run(command, capture_output=True, text=True, **kwargs)
        return result.returncode, result.stdout + result.stderr
    except (OSError, ValueError) as error:
        return 127, str(error)


def version_of(text):
    match = re.search(r"(\d+)\.(\d+)(?:\.(\d+))?", text)
    return tuple(int(part or 0) for part in match.groups()) if match else None


class Dependency:
    """A named check plus the package that provides it on each package manager."""

    def __init__(self, name, check, packages, required=True, tests_only=False, note=""):
        self.name, self.check, self.packages = name, check, packages
        self.required, self.tests_only, self.note = required, tests_only, note


# ---------------------------------------------------------------- checks

def has_program(name):
    return lambda: (shutil.which(name) is not None, shutil.which(name) or "not found")


def check_cmake():
    code, out = run(["cmake", "--version"])
    found = version_of(out) if code == 0 else None
    if not found:
        return False, "not found"
    return found[:2] >= REQUIRED_CMAKE, ".".join(map(str, found)) + f" (need >= {REQUIRED_CMAKE[0]}.{REQUIRED_CMAKE[1]})"


def check_dotnet():
    code, out = run(["dotnet", "--list-sdks"])
    if code != 0:
        return False, "not found"
    majors = [int(line.split(".")[0]) for line in out.splitlines() if line[:1].isdigit()]
    ok = any(major >= REQUIRED_DOTNET_MAJOR for major in majors)
    return ok, ", ".join(line.split(" ")[0] for line in out.splitlines() if line.strip()) or "no SDK"


def check_pkgconfig(module):
    def check():
        if shutil.which("pkg-config") is None:
            return False, "pkg-config not found"
        code, out = run(["pkg-config", "--modversion", module])
        return code == 0, out.strip() if code == 0 else "not found"
    return check


def check_system_ffmpeg():
    versions = []
    for library in FFMPEG_LIBRARIES:
        module = "lib" + library
        code, version = run(["pkg-config", "--modversion", module])
        if code != 0:
            return False, f"{module} development package not found"
        code, directory = run(["pkg-config", "--variable=libdir", module])
        try:
            if code != 0:
                return False, f"{module} library directory not found"
            shared = ctypes.CDLL(str(Path(directory.strip()) / (module + ".so")))
            license_name = getattr(shared, library + "_license")
            license_name.restype = ctypes.c_char_p
            if not license_name().startswith(b"LGPL"):
                return False, f"{module} is not an LGPL build"
        except (OSError, AttributeError):
            return False, f"{module} shared library not found"
        versions.append(version.strip())
    return True, " / ".join(versions)


def ffmpeg_windows_files(directory):
    return all((directory / "lib" / (library + ".lib")).is_file() and
               any((directory / "bin").glob(library + "-*.dll")) for library in FFMPEG_LIBRARIES)


def check_windows_ffmpeg():
    stamp = FFMPEG_DIR / ".sha256"
    if stamp.is_file() and stamp.read_text().strip() == FFMPEG_SHA256 and ffmpeg_windows_files(FFMPEG_DIR):
        return True, FFMPEG_ARCHIVE
    return False, "pinned shared build not installed"


def install_windows_ffmpeg():
    if check_windows_ffmpeg()[0]:
        return True
    try:
        with tempfile.TemporaryDirectory(prefix="ffmpeg-", dir=FFMPEG_DIR.parent) as temp:
            archive = Path(temp) / "ffmpeg.zip"
            digest = hashlib.sha256()
            with urllib.request.urlopen(FFMPEG_URL, timeout=60) as response, archive.open("wb") as output:
                for chunk in iter(lambda: response.read(1024 * 1024), b""):
                    digest.update(chunk)
                    output.write(chunk)
            if digest.hexdigest() != FFMPEG_SHA256:
                raise ValueError("FFmpeg archive SHA-256 mismatch")
            with zipfile.ZipFile(archive) as packed:
                packed.extractall(temp)
            extracted = Path(temp) / FFMPEG_ARCHIVE
            if not ffmpeg_windows_files(extracted):
                raise ValueError("FFmpeg archive is missing MSVC import libraries or DLLs")
            (extracted / ".sha256").write_text(FFMPEG_SHA256 + "\n")
            # Keep an existing usable directory intact until the replacement is ready.
            previous = Path(temp) / "previous"
            if FFMPEG_DIR.exists():
                FFMPEG_DIR.rename(previous)
            try:
                extracted.rename(FFMPEG_DIR)
            except OSError:
                if previous.exists():
                    previous.rename(FFMPEG_DIR)
                raise
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        print(f"Optional FFmpeg install failed: {error}", file=sys.stderr)
        return False
    return True


def steam_audio_platform():
    if platform.machine().lower() not in ("amd64", "x86_64"):
        return None, ()
    if platform.system() == "Windows":
        return "windows-x64", ("phonon.dll", "phonon.lib")
    if platform.system() == "Linux":
        return "linux-x64", ("libphonon.so",)
    return None, ()


def steam_audio_files(directory, target, libraries):
    headers = ("phonon.h", "phonon_interfaces.h", "phonon_version.h")
    return ((directory / "THIRDPARTY.md").is_file()
            and all((directory / "include" / name).is_file() for name in headers)
            and all((directory / "lib" / target / name).is_file() for name in libraries))


def check_steam_audio():
    target, libraries = steam_audio_platform()
    if not target:
        return False, "the pinned SDK requires x64 Linux or Windows"
    stamp = STEAM_AUDIO_DIR / ".sha256"
    if (stamp.is_file() and stamp.read_text().strip() == STEAM_AUDIO_SHA256
            and steam_audio_files(STEAM_AUDIO_DIR, target, libraries)):
        return True, STEAM_AUDIO_VERSION
    return False, "pinned Steam Audio SDK not installed"


def install_steam_audio():
    target, libraries = steam_audio_platform()
    if not target:
        return False
    if check_steam_audio()[0]:
        return True
    try:
        with tempfile.TemporaryDirectory(prefix="steamaudio-", dir=STEAM_AUDIO_DIR.parent) as temp:
            archive = Path(temp) / "steamaudio.zip"
            digest = hashlib.sha256()
            with urllib.request.urlopen(STEAM_AUDIO_URL, timeout=60) as response, archive.open("wb") as output:
                for chunk in iter(lambda: response.read(1024 * 1024), b""):
                    digest.update(chunk)
                    output.write(chunk)
            if digest.hexdigest() != STEAM_AUDIO_SHA256:
                raise ValueError("Steam Audio archive SHA-256 mismatch")
            with zipfile.ZipFile(archive) as packed:
                for name in packed.namelist():
                    if (name.startswith("steamaudio/include/") or name == "steamaudio/THIRDPARTY.md"
                            or name in {f"steamaudio/lib/{target}/{library}" for library in libraries}):
                        packed.extract(name, temp)
            extracted = Path(temp) / "steamaudio"
            if not steam_audio_files(extracted, target, libraries):
                raise ValueError("Steam Audio archive is missing headers, libraries or license notices")
            (extracted / ".sha256").write_text(STEAM_AUDIO_SHA256 + "\n")
            previous = Path(temp) / "previous"
            if STEAM_AUDIO_DIR.exists():
                STEAM_AUDIO_DIR.rename(previous)
            try:
                extracted.rename(STEAM_AUDIO_DIR)
            except OSError:
                if previous.exists():
                    previous.rename(STEAM_AUDIO_DIR)
                raise
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        print(f"Optional Steam Audio install failed: {error}", file=sys.stderr)
        return False
    return True


def check_library(*names):
    """Looks for a library file in the usual Linux locations (no pkg-config file for some packages)."""
    def check():
        roots = ["/usr/lib", "/usr/lib64", "/usr/local/lib", "/usr/lib/x86_64-linux-gnu", "/usr/lib/aarch64-linux-gnu"]
        for root in roots:
            for name in names:
                if Path(root, name).exists():
                    return True, str(Path(root, name))
        return False, "not found"
    return check


def check_header(relative):
    def check():
        for root in ["/usr/include", "/usr/local/include"]:
            if Path(root, relative).exists():
                return True, str(Path(root, relative))
        return False, "not found"
    return check


def check_googletest():
    """A system GoogleTest, or the copy Configure pulls into test/vendor/googletest (a tests-repo submodule)."""
    if platform.system() != "Windows":
        ok, detail = check_header("gtest/gtest.h")()
        if ok:
            return ok, detail  # CMake prefers the system package too.
    if (VENDORED_GTEST / "CMakeLists.txt").is_file():
        return True, str(VENDORED_GTEST.relative_to(ROOT))
    if not (TEST_DIR / "CMakeLists.txt").is_file():
        return False, "test/ submodule not pulled"
    return False, "Configure pulls it into test/vendor/googletest"


def update_test_vendor():
    """Once the tests submodule is checked out, pulls its own submodules (GoogleTest) at its pinned commits."""
    if not (TEST_DIR / "CMakeLists.txt").is_file():
        return True
    print(bold("$ git -C test submodule update --init --recursive"))
    return subprocess.call(["git", "submodule", "update", "--init", "--recursive"], cwd=TEST_DIR) == 0


def check_lavapipe():
    for root in ["/usr/share/vulkan/icd.d", "/etc/vulkan/icd.d"]:
        for icd in sorted(Path(root).glob("lvp_icd*.json")):
            return True, str(icd)
    return False, "lvp_icd.json not found"


def check_submodules():
    code, out = run(["git", "submodule", "status", "--recursive"], cwd=ROOT)
    if code != 0:
        return False, "git submodule status failed"
    missing = [line.split()[1] for line in out.splitlines() if line.startswith("-")]
    return not missing, "initialized" if not missing else "missing: " + ", ".join(missing)


def check_visual_studio():
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                   "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not vswhere.exists():
        return False, "Visual Studio Installer not found"
    code, out = run([str(vswhere), "-latest", "-version", "[17.0,18.0)", "-products", "*",
                     "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "displayName"])
    return code == 0 and bool(out.strip()), out.strip() or "VS 2022 with C++ tools not found"


def check_vulkan_sdk():
    sdk = os.environ.get("VULKAN_SDK")
    if not sdk or not Path(sdk).exists():
        return False, "VULKAN_SDK is not set (a new terminal is needed after installing)"
    return True, sdk


# ---------------------------------------------------------------- dependency tables

def linux_dependencies():
    return [
        Dependency("Git", has_program("git"), {"pacman": "git", "apt": "git", "dnf": "git"}),
        Dependency("CMake", check_cmake, {"pacman": "cmake", "apt": "cmake", "dnf": "cmake"}),
        Dependency("Ninja", has_program("ninja"), {"pacman": "ninja", "apt": "ninja-build", "dnf": "ninja-build"}),
        Dependency("GCC (g++)", has_program("g++"), {"pacman": "gcc", "apt": "g++", "dnf": "gcc-c++"}),
        Dependency("pkg-config", has_program("pkg-config"), {"pacman": "pkgconf", "apt": "pkg-config", "dnf": "pkgconf-pkg-config"}),
        Dependency("Vulkan loader and headers", check_pkgconfig("vulkan"),
                   {"pacman": "vulkan-icd-loader vulkan-headers", "apt": "libvulkan-dev", "dnf": "vulkan-loader-devel vulkan-headers"}),
        Dependency("shaderc", check_library("libshaderc_combined.a"),
                   {"pacman": "shaderc", "apt": "libshaderc-dev", "dnf": "libshaderc-devel"},
                   note="Debian/Ubuntu may lack libshaderc_combined.a; use the LunarG Vulkan SDK there"),
        Dependency("glslang", check_library("libglslang.so", "libglslang.a"),
                   {"pacman": "glslang", "apt": "glslang-dev", "dnf": "glslang-devel"}),
        Dependency("SPIRV-Cross", check_library("libspirv-cross-glsl.a", "libspirv-cross-glsl.so"),
                   {"pacman": "spirv-cross", "apt": "libspirv-cross-c-shared-dev spirv-cross", "dnf": "spirv-cross-devel"}),
        Dependency("SPIRV-Tools", check_library("libSPIRV-Tools.so", "libSPIRV-Tools.a"),
                   {"pacman": "spirv-tools", "apt": "spirv-tools", "dnf": "spirv-tools-devel"}),
        Dependency("Assimp", check_header("assimp/Importer.hpp"),
                   {"pacman": "assimp", "apt": "libassimp-dev", "dnf": "assimp-devel"}),
        Dependency("X11 (GLFW)", check_pkgconfig("x11"),
                   {"pacman": "libx11 libxrandr libxinerama libxcursor libxi",
                    "apt": "libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev",
                    "dnf": "libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel"}),
        Dependency("Wayland (GLFW)", check_pkgconfig("wayland-client"),
                   {"pacman": "wayland wayland-protocols libxkbcommon",
                    "apt": "libwayland-dev wayland-protocols libxkbcommon-dev",
                    "dnf": "wayland-devel wayland-protocols-devel libxkbcommon-devel"}),
        Dependency(".NET SDK 10 (C# scripting)", check_dotnet,
                   {"pacman": "dotnet-sdk", "apt": "dotnet-sdk-10.0", "dnf": "dotnet-sdk-10.0"}, required=False,
                   note="without it the engine builds with C# scripting disabled"),
        Dependency("FFmpeg (video decoding)", check_system_ffmpeg,
                   {"pacman": "ffmpeg", "apt": "libavformat-dev libavcodec-dev libavutil-dev",
                    "dnf": "ffmpeg-free-devel"}, required=False,
                   note="requires a shared LGPL build; some distributions ship GPL builds"),
        Dependency("Steam Audio (binaural HRTF)", check_steam_audio, {}, required=False,
                   note="downloads the SHA-256-pinned x64 SDK"),
        Dependency("GoogleTest", check_googletest,
                   {"pacman": "gtest", "apt": "libgtest-dev", "dnf": "gtest-devel"}, required=False, tests_only=True,
                   note="Configure pulls it into test/vendor/googletest when the package is missing"),
        Dependency("Software Vulkan (lavapipe)", check_lavapipe,
                   {"pacman": "vulkan-swrast", "apt": "mesa-vulkan-drivers", "dnf": "mesa-vulkan-drivers"}, tests_only=True,
                   note="GPU tests run on lavapipe; never on the real GPU by default"),
    ]


def windows_dependencies():
    return [
        Dependency("Git", has_program("git"), {"winget": "Git.Git"}),
        Dependency("CMake", check_cmake, {"winget": "Kitware.CMake"}),
        Dependency("Visual Studio 2022 (C++)", check_visual_studio,
                   {"winget": "Microsoft.VisualStudio.2022.Community --override \"--add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended --passive\""}),
        Dependency("Vulkan SDK", check_vulkan_sdk, {"winget": "KhronosGroup.VulkanSDK"}),
        Dependency(".NET SDK 10 (C# scripting)", check_dotnet, {"winget": "Microsoft.DotNet.SDK.10"}, required=False,
                   note="without it the engine builds with C# scripting disabled"),
        Dependency("FFmpeg (video decoding)", check_windows_ffmpeg, {}, required=False,
                   note="downloads the SHA-256-pinned LGPL shared build"),
        Dependency("Steam Audio (binaural HRTF)", check_steam_audio, {}, required=False,
                   note="downloads the SHA-256-pinned x64 SDK"),
        Dependency("GoogleTest", check_googletest, {}, required=False, tests_only=True,
                   note="Configure pulls it into test/vendor/googletest"),
    ]


# ---------------------------------------------------------------- package managers

def package_manager():
    if platform.system() == "Windows":
        return "winget" if shutil.which("winget") else None
    os_release = {}
    try:
        for line in Path("/etc/os-release").read_text().splitlines():
            key, _, value = line.partition("=")
            os_release[key] = value.strip('"')
    except OSError:
        pass
    family = " ".join([os_release.get("ID", ""), os_release.get("ID_LIKE", "")])
    for name, ids in (("pacman", ("arch",)), ("apt", ("debian", "ubuntu")), ("dnf", ("fedora", "rhel"))):
        if any(i in family.split() for i in ids) and shutil.which(name):
            return name
    return None


def install_commands(manager, packages):
    if manager == "winget":
        return [f"winget install --exact --accept-package-agreements --accept-source-agreements --id {p}" for p in packages]
    joined = " ".join(packages)
    return {
        "pacman": [f"sudo pacman -S --needed {joined}"],
        "apt": ["sudo apt-get update", f"sudo apt-get install -y {joined}"],
        "dnf": [f"sudo dnf install -y {joined}"],
    }[manager]


def ask(question, assume_yes):
    if assume_yes:
        return True
    try:
        return input(f"{question} [y/N] ").strip().lower() in ("y", "yes")
    except EOFError:
        return False


# ---------------------------------------------------------------- shared steps

class Style:
    """ANSI colours, only when writing to a terminal that supports them (NO_COLOR disables)."""
    enabled = False

    @classmethod
    def setup(cls):
        if os.environ.get("NO_COLOR") or not sys.stdout.isatty():
            return
        if platform.system() == "Windows":
            os.system("")  # Enables virtual-terminal processing on Windows 10+ consoles.
        cls.enabled = True

    @classmethod
    def paint(cls, text, code):
        return f"\033[{code}m{text}\033[0m" if cls.enabled else text


def green(text): return Style.paint(text, "32")
def red(text): return Style.paint(text, "31")
def yellow(text): return Style.paint(text, "33")
def dim(text): return Style.paint(text, "2")
def bold(text): return Style.paint(text, "1")


def status_label(dependency, ok):
    if ok:
        return green("OK      ")
    return red("MISSING ") if dependency.required else yellow("optional")


def packages_for(manager, dependencies):
    packages = []
    for dependency in dependencies:
        package = dependency.packages.get(manager)
        for item in ([package] if manager == "winget" else package.split()) if package else []:
            if item not in packages:
                packages.append(item)
    return packages


def dependency_commands(manager, dependencies):
    packages = packages_for(manager, dependencies)
    commands = install_commands(manager, packages) if manager and packages else []
    if any(d.check is check_windows_ffmpeg for d in dependencies):
        commands.append(subprocess.list2cmdline([sys.executable, str(Path(__file__).resolve()), "--install-ffmpeg"]))
    if steam_audio_platform()[0] and any(d.check is check_steam_audio for d in dependencies):
        quote_command = subprocess.list2cmdline if platform.system() == "Windows" else shlex.join
        commands.append(quote_command([sys.executable, str(Path(__file__).resolve()), "--install-steam-audio"]))
    return commands


def run_install(commands, windows):
    for command in commands:
        print(bold(f"$ {command}"))
        if subprocess.call(command, shell=True) != 0:
            print(red(f"Failed: {command}"))
            return False
    if windows:
        print(yellow("Open a new terminal so PATH and VULKAN_SDK changes take effect, then run Setup again."))
    return True


def configure(preset, build):
    if not check_submodules()[0]:
        print(bold("$ git submodule update --init --recursive"))
        if subprocess.call(["git", "submodule", "update", "--init", "--recursive"], cwd=ROOT) != 0:
            return False
    # The engine's submodule check misses a tests checkout that predates its GoogleTest submodule.
    if not update_test_vendor():
        return False
    print(bold(f"$ cmake --preset {preset}"))
    if subprocess.call(["cmake", "--preset", preset], cwd=ROOT) != 0:
        return False
    if build:
        jobs = str(os.cpu_count() or 2)
        print(bold(f"$ cmake --build build/{preset} --config Debug --target KansEditor --parallel {jobs}"))
        if subprocess.call(["cmake", "--build", f"build/{preset}", "--config", "Debug", "--target", "KansEditor",
                            "--parallel", jobs], cwd=ROOT) != 0:
            return False
    return True


# ---------------------------------------------------------------- interactive menu

def pause():
    try:
        input(dim("\nPress Enter to return to the menu..."))
    except (EOFError, KeyboardInterrupt):
        pass


def load_prompts():
    """Imports SetupPrompt with the vendored prompt_toolkit wheels, or returns None."""
    if sys.version_info < (3, 10):
        return None
    sys.path[:0] = [str(wheel) for wheel in sorted(THIRD_PARTY.glob("*.whl"))] + [str(Path(__file__).resolve().parent)]
    try:
        import SetupPrompt
        return SetupPrompt
    except ImportError:
        return None


def print_status(rows, manager):
    print(bold("Kans3D setup") + dim(f"  {platform.system()} {platform.machine()} · "
                                     f"package manager: {manager or 'none found'} · "
                                     f"submodules: {check_submodules()[1]}"))
    print()
    for dependency, ok, detail in rows:
        note = dim(f" — {dependency.note}") if not ok and dependency.note else ""
        print(f"  {status_label(dependency, ok)} {dependency.name:<28} {dim(detail)}{note}")
    print()


def interactive(prompts, preset, tests, build):
    windows = platform.system() == "Windows"
    manager = package_manager()
    presets = ["windows-msvc", "windows-clangd"] if windows else ["linux-gcc", "linux-clang"]
    Option = prompts.Option
    selected = None
    while True:
        dependencies = windows_dependencies() if windows else linux_dependencies()
        dependencies = [d for d in dependencies if tests or not d.tests_only]
        rows = sorted(((d,) + tuple(d.check()) for d in dependencies), key=lambda r: (r[1], not r[0].required))
        missing = [d for d, ok, _ in rows if not ok]
        if selected is None:
            selected = {d.name for d in missing}
        selected &= {d.name for d in missing}
        required = [d for d in missing if d.required]
        chosen = [d for d in missing if d.name in selected]
        commands = dependency_commands(manager, chosen)

        if Style.enabled:
            print("\033[2J\033[H", end="")
        print_status(rows, manager)

        configure_text = f"cmake --preset {preset}" + (" and build KansEditor" if build else "")
        options = []
        if missing:
            options.append(Option(f"Choose what to install ({len(chosen)} of {len(missing)} selected)",
                                  ", ".join(d.name for d in chosen) or "nothing selected", True, "choose"))
            options.append(Option("Install selected", " && ".join(commands) if commands else
                                  ("no supported package manager" if manager is None else "nothing selected"),
                                  bool(commands), "install"))
        options.append(Option("Configure", configure_text if not required else
                              "install the missing required dependencies first", not required, "configure"))
        options.append(Option(f"Preset: {preset}", "switch to " + ", ".join(p for p in presets if p != preset),
                              True, "preset"))
        options.append(Option(f"Test dependencies: {'on' if tests else 'off'}", "GoogleTest, software Vulkan",
                              True, "tests"))
        options.append(Option(f"Build after configure: {'on' if build else 'off'}", "KansEditor, Debug",
                              True, "build"))
        options.append(Option("Re-check", "", True, "recheck"))
        options.append(Option("Quit", "", True, "quit"))

        action = prompts.choose("What do you want to do?", options)
        if action in (None, "quit"):
            return 0
        if action == "choose":
            picked = prompts.choose("Install which missing dependencies?",
                                    [Option(d.name, ("required" if d.required else "optional") +
                                            (f" — {d.note}" if d.note else ""), True, d.name) for d in missing],
                                    multi=True, checked=selected)
            if picked is not None:
                selected = set(picked)
        elif action == "install":
            print()
            ok = run_install(commands, windows)
            if ok and windows:
                return 0
            print(green("Install finished.") if ok else red("Install failed; see the output above."))
            pause()
        elif action == "configure":
            print()
            ok = configure(preset, build)
            print(green("\nDone.") if ok else red("\nStopped."))
            return 0 if ok else 1
        elif action == "preset":
            preset = presets[(presets.index(preset) + 1) % len(presets)] if preset in presets else presets[0]
        elif action == "tests":
            tests, selected = not tests, None
        elif action == "build":
            build = not build


# ---------------------------------------------------------------- non-interactive

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--install-ffmpeg", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--install-steam-audio", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--check", action="store_true", help="report only, install nothing")
    parser.add_argument("--yes", action="store_true", help="install everything missing without asking")
    parser.add_argument("--tests", action="store_true", help="include test dependencies")
    parser.add_argument("--build", action="store_true", help="build KansEditor (Debug) after configuring")
    parser.add_argument("--preset", help="CMake configure preset (default: linux-gcc / windows-msvc)")
    parser.add_argument("--non-interactive", action="store_true", help="no menu, even in a terminal")
    args = parser.parse_args()
    Style.setup()

    windows = platform.system() == "Windows"
    if args.install_ffmpeg:
        sys.exit(0 if windows and install_windows_ffmpeg() else 1)
    if args.install_steam_audio:
        sys.exit(0 if install_steam_audio() else 1)
    preset = args.preset or ("windows-msvc" if windows else "linux-gcc")
    terminal = sys.stdin.isatty() and sys.stdout.isatty()
    if terminal and not (args.check or args.yes or args.non_interactive):
        prompts = load_prompts()
        if prompts:
            sys.exit(interactive(prompts, preset, args.tests, args.build))
        print(yellow("Interactive menu needs Python 3.10+; using the plain mode.\n"))

    dependencies = windows_dependencies() if windows else linux_dependencies()
    dependencies = [d for d in dependencies if args.tests or not d.tests_only]
    manager = package_manager()

    print(f"Kans3D setup — {platform.system()} {platform.machine()}, package manager: {manager or 'none found'}\n")
    missing = []
    for dependency in dependencies:
        ok, detail = dependency.check()
        print(f"  {status_label(dependency, ok)} {dependency.name:<30} {detail}")
        if not ok:
            missing.append(dependency)
            if dependency.note:
                print(f"           {dim(dependency.note)}")
    submodules_ok, detail = check_submodules()
    print(f"  {green('OK      ') if submodules_ok else red('MISSING ')} {'Git submodules':<30} {detail}")

    if args.check:
        required_missing = [d for d in missing if d.required] or ([] if submodules_ok else ["submodules"])
        sys.exit(1 if required_missing else 0)

    commands = dependency_commands(manager, missing)
    if missing and manager is None:
        print("\nNo supported package manager found; install system packages manually.")
    if commands:
        print("\nInstall commands:")
        for command in commands:
            print(f"  {command}")
        if ask("Run them now?", args.yes):
            if not run_install(commands, windows):
                sys.exit(1)
            if windows:
                sys.exit(0)

    still_missing = [d.name for d in dependencies if d.required and not d.check()[0]]
    if still_missing:
        print("\nStill missing: " + ", ".join(still_missing) + ". Configure skipped.")
        sys.exit(1)
    if not configure(preset, args.build):
        sys.exit(1)
    print("\nDone.")


if __name__ == "__main__":
    main()
