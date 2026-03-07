#!/bin/bash
# csend Development Environment Setup (Full-Chain Bootstrap)
#
# csend sits at the top of the dependency chain:
#   Retro68 (toolchain) -> clog (logging) -> peertalk (networking) -> csend (chat app)
#
# This script checks all dependencies and offers to clone/build any missing ones.
# Works on Ubuntu 24/25 and macOS with Homebrew.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RETRO68_TOOLCHAIN="${RETRO68_TOOLCHAIN:-$HOME/Retro68-build/toolchain}"
RETRO68_SRC="${RETRO68_SRC:-$HOME/Retro68}"
CLOG_DIR="${CLOG_DIR:-$HOME/clog}"
PEERTALK_DIR="${PEERTALK_DIR:-$HOME/peertalk}"

echo "csend Development Setup (Full-Chain Bootstrap)"
echo "==============================================="
echo ""

# ── Helpers ──────────────────────────────────────────────────────

check_tool() {
    if command -v "$1" &>/dev/null; then
        echo "  [ok] $1 ($(command -v "$1"))"
        return 0
    else
        echo "  [!!] $1 not found"
        return 1
    fi
}

ensure_bashrc_export() {
    local var_name="$1" var_value="$2"
    if ! grep -q "export ${var_name}=" "$HOME/.bashrc" 2>/dev/null; then
        echo "" >> "$HOME/.bashrc"
        echo "# Added by csend/setup.sh" >> "$HOME/.bashrc"
        echo "export ${var_name}=\"${var_value}\"" >> "$HOME/.bashrc"
        echo "  [ok] Added ${var_name} to ~/.bashrc"
    else
        echo "  [ok] ${var_name} already in ~/.bashrc"
    fi
}

install_packages() {
    if command -v apt-get &>/dev/null; then
        sudo apt-get update
        sudo apt-get install -y "$@"
    elif command -v brew &>/dev/null; then
        brew install "$@"
    else
        echo "  [!!] No supported package manager found (need apt or brew)"
        exit 1
    fi
}

ask_yes_no() {
    local prompt="$1"
    local response
    read -rp "$prompt [y/N] " response
    [[ "$response" =~ ^[Yy]$ ]]
}

# ── Check core prerequisites ────────────────────────────────────

echo "Checking prerequisites..."
MISSING=0
check_tool gcc || MISSING=1
check_tool cmake || MISSING=1
check_tool make || MISSING=1
check_tool git || MISSING=1

if [ "$MISSING" -eq 1 ]; then
    echo ""
    if ask_yes_no "Install missing build tools?"; then
        if command -v apt-get &>/dev/null; then
            install_packages build-essential cmake git
        elif command -v brew &>/dev/null; then
            install_packages gcc cmake git make
        fi
    else
        echo "Cannot continue without build tools."
        exit 1
    fi
fi

# ── Check/setup Retro68 ─────────────────────────────────────────

echo ""
echo "Checking Retro68 cross-compiler..."
if [ -x "$RETRO68_TOOLCHAIN/bin/m68k-apple-macos-gcc" ]; then
    echo "  [ok] m68k-apple-macos-gcc"
elif [ -d "$RETRO68_SRC" ] && [ ! -x "$RETRO68_TOOLCHAIN/bin/m68k-apple-macos-gcc" ]; then
    echo "  [--] Retro68 source found but not built"
    if ask_yes_no "Build Retro68 toolchain? (takes 1-2 hours)"; then
        echo "  Installing Retro68 build prerequisites..."
        if command -v apt-get &>/dev/null; then
            install_packages build-essential cmake git unzip \
                libgmp-dev libmpfr-dev libmpc-dev libboost-all-dev \
                bison flex texinfo ruby
        elif command -v brew &>/dev/null; then
            install_packages cmake gmp mpfr libmpc boost bison flex texinfo ruby
        fi

        echo "  Initializing submodules..."
        (cd "$RETRO68_SRC" && git submodule update --init --recursive)

        echo "  Building toolchain..."
        mkdir -p "$HOME/Retro68-build"
        (cd "$HOME/Retro68-build" && "$RETRO68_SRC/build-toolchain.bash")

        ensure_bashrc_export "RETRO68_TOOLCHAIN" "$RETRO68_TOOLCHAIN"
        ensure_bashrc_export "RETRO68_SRC" "$RETRO68_SRC"
        ensure_bashrc_export "PATH" "\$RETRO68_TOOLCHAIN/bin:\$PATH"
    else
        echo "  [--] Skipping Retro68 build (cross-compilation unavailable)"
    fi
else
    echo "  [--] Retro68 not found"
    if ask_yes_no "Clone and build Retro68? (takes 1-2 hours)"; then
        echo "  Installing Retro68 build prerequisites..."
        if command -v apt-get &>/dev/null; then
            install_packages build-essential cmake git unzip \
                libgmp-dev libmpfr-dev libmpc-dev libboost-all-dev \
                bison flex texinfo ruby
        elif command -v brew &>/dev/null; then
            install_packages cmake gmp mpfr libmpc boost bison flex texinfo ruby
        fi

        echo "  Cloning Retro68..."
        git clone https://github.com/matthewdeaves/Retro68.git "$RETRO68_SRC"
        (cd "$RETRO68_SRC" && git submodule update --init --recursive)

        echo "  Building toolchain..."
        mkdir -p "$HOME/Retro68-build"
        (cd "$HOME/Retro68-build" && "$RETRO68_SRC/build-toolchain.bash")

        ensure_bashrc_export "RETRO68_TOOLCHAIN" "$RETRO68_TOOLCHAIN"
        ensure_bashrc_export "RETRO68_SRC" "$RETRO68_SRC"
    else
        echo "  [--] Skipping Retro68 (cross-compilation unavailable)"
    fi
fi

if [ -x "$RETRO68_TOOLCHAIN/bin/powerpc-apple-macos-gcc" ]; then
    echo "  [ok] powerpc-apple-macos-gcc"
else
    echo "  [--] powerpc-apple-macos-gcc not found (PPC builds unavailable)"
fi

# ── Check/setup clog ────────────────────────────────────────────

echo ""
echo "Checking clog library..."
if [ -f "$CLOG_DIR/build/libclog.a" ]; then
    echo "  [ok] libclog.a found at $CLOG_DIR/build/libclog.a"
elif [ -f "$CLOG_DIR/setup.sh" ]; then
    echo "  [--] clog source found but not built. Running clog/setup.sh..."
    (cd "$CLOG_DIR" && bash setup.sh)
elif [ -f "$CLOG_DIR/CMakeLists.txt" ]; then
    echo "  [--] clog source found but not built. Building..."
    mkdir -p "$CLOG_DIR/build"
    (cd "$CLOG_DIR/build" && cmake .. && make)
    if [ -f "$CLOG_DIR/build/libclog.a" ]; then
        echo "  [ok] clog built successfully"
    else
        echo "  [!!] clog build failed"
        exit 1
    fi
else
    echo "  [!!] clog not found at $CLOG_DIR"
    if ask_yes_no "Clone and build clog?"; then
        git clone https://github.com/matthewdeaves/clog.git "$CLOG_DIR"
        if [ -f "$CLOG_DIR/setup.sh" ]; then
            (cd "$CLOG_DIR" && bash setup.sh)
        else
            mkdir -p "$CLOG_DIR/build"
            (cd "$CLOG_DIR/build" && cmake .. && make)
        fi
    else
        echo "  [!!] Cannot continue without clog"
        exit 1
    fi
fi

# ── Check/setup peertalk ────────────────────────────────────────

echo ""
echo "Checking peertalk SDK..."
if [ -f "$PEERTALK_DIR/include/peertalk.h" ]; then
    echo "  [ok] peertalk source found at $PEERTALK_DIR"
else
    echo "  [!!] peertalk not found at $PEERTALK_DIR"
    if ask_yes_no "Clone peertalk?"; then
        git clone https://github.com/matthewdeaves/peertalk.git "$PEERTALK_DIR"
        if [ -f "$PEERTALK_DIR/setup.sh" ]; then
            (cd "$PEERTALK_DIR" && bash setup.sh)
        fi
    else
        echo "  [!!] Cannot continue without peertalk"
        exit 1
    fi
fi

# ── Check MPW Interfaces ────────────────────────────────────────

echo ""
echo "Checking MPW Interfaces..."
MPW_CINCLUDES="$RETRO68_SRC/InterfacesAndLibraries/MPW_Interfaces/Interfaces&Libraries/Interfaces/CIncludes"
MPW_ZIP="$RETRO68_SRC/resources/MPW_Interfaces.zip"

if [ -f "$MPW_CINCLUDES/MacTCP.h" ] && [ -f "$MPW_CINCLUDES/OpenTransport.h" ]; then
    echo "  [ok] MacTCP.h and OpenTransport.h found"
elif [ -f "$MPW_ZIP" ]; then
    echo "  [--] Extracting MPW Interfaces from Retro68..."
    mkdir -p "$RETRO68_SRC/InterfacesAndLibraries"
    unzip -o "$MPW_ZIP" -d "$RETRO68_SRC/InterfacesAndLibraries/"
    echo "  [ok] MPW Interfaces extracted"
else
    echo "  [--] MPW Interfaces not available (cross-compilation may fail)"
fi

# ── Register environment variables ──────────────────────────────

echo ""
echo "Checking environment..."
ensure_bashrc_export "CSEND_DIR" "$SCRIPT_DIR"
ensure_bashrc_export "CLOG_DIR" "$CLOG_DIR"
ensure_bashrc_export "PEERTALK_DIR" "$PEERTALK_DIR"
if [ -x "$RETRO68_TOOLCHAIN/bin/m68k-apple-macos-gcc" ]; then
    ensure_bashrc_export "RETRO68_TOOLCHAIN" "$RETRO68_TOOLCHAIN"
    ensure_bashrc_export "RETRO68_SRC" "$RETRO68_SRC"
fi

export CLOG_DIR PEERTALK_DIR CSEND_DIR="$SCRIPT_DIR"

# ── Build csend targets ─────────────────────────────────────────

echo ""
echo "Building csend POSIX target..."
mkdir -p "$SCRIPT_DIR/build"
if (cd "$SCRIPT_DIR/build" && cmake .. -DCLOG_DIR="$CLOG_DIR" -DPEERTALK_DIR="$PEERTALK_DIR" && make) 2>&1; then
    echo "  [ok] POSIX build succeeded"
else
    echo "  [!!] POSIX build failed"
    exit 1
fi

# Cross-compile targets (optional)
if [ -x "$RETRO68_TOOLCHAIN/bin/m68k-apple-macos-gcc" ]; then
    echo ""
    echo "Building csend 68k MacTCP target..."
    M68K_TOOLCHAIN="$RETRO68_TOOLCHAIN/m68k-apple-macos/cmake/retro68.toolchain.cmake"
    mkdir -p "$SCRIPT_DIR/build-68k"
    if (cd "$SCRIPT_DIR/build-68k" && cmake .. \
        -DCMAKE_TOOLCHAIN_FILE="$M68K_TOOLCHAIN" \
        -DPT_PLATFORM=MACTCP \
        -DCLOG_DIR="$CLOG_DIR" -DCLOG_LIB_DIR="$CLOG_DIR/build-m68k" \
        -DPEERTALK_DIR="$PEERTALK_DIR" && make) 2>&1; then
        echo "  [ok] 68k MacTCP build succeeded"
    else
        echo "  [!!] 68k MacTCP build failed"
    fi
fi

if [ -x "$RETRO68_TOOLCHAIN/bin/powerpc-apple-macos-gcc" ]; then
    echo ""
    echo "Building csend PPC OT target..."
    PPC_TOOLCHAIN="$RETRO68_TOOLCHAIN/powerpc-apple-macos/cmake/retroppc.toolchain.cmake"
    mkdir -p "$SCRIPT_DIR/build-ppc-ot"
    if (cd "$SCRIPT_DIR/build-ppc-ot" && cmake .. \
        -DCMAKE_TOOLCHAIN_FILE="$PPC_TOOLCHAIN" \
        -DPT_PLATFORM=OT \
        -DCLOG_DIR="$CLOG_DIR" -DCLOG_LIB_DIR="$CLOG_DIR/build-ppc" \
        -DPEERTALK_DIR="$PEERTALK_DIR" && make) 2>&1; then
        echo "  [ok] PPC OT build succeeded"
    else
        echo "  [!!] PPC OT build failed"
    fi
fi

# ── Summary ─────────────────────────────────────────────────────

echo ""
echo "==============================================="
echo "Setup complete!"
echo ""
echo "  Dependency chain:"
echo "    Retro68:   $RETRO68_TOOLCHAIN"
echo "    clog:      $CLOG_DIR"
echo "    peertalk:  $PEERTALK_DIR"
echo "    csend:     $SCRIPT_DIR"
echo ""
echo "  Builds:"
[ -f "$SCRIPT_DIR/build/csend-posix" ] && \
echo "    POSIX:     $SCRIPT_DIR/build/csend-posix"
[ -d "$SCRIPT_DIR/build-68k" ] && \
echo "    68k:       $SCRIPT_DIR/build-68k/"
[ -d "$SCRIPT_DIR/build-ppc-ot" ] && \
echo "    PPC OT:    $SCRIPT_DIR/build-ppc-ot/"
echo ""
echo "Run 'source ~/.bashrc' to pick up env vars in this shell."
echo ""
echo "Usage: ./build/csend-posix <username>"
echo ""
