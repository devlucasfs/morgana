#!/usr/bin/env bash
set -Eeuo pipefail

# ESP-IDF installer
# Arch Linux / Manjaro
# Target: ESP32-S3 (Xtensa LX7)

ESP_DIR="${HOME}/esp"
IDF_DIR="${ESP_DIR}/esp-idf"
IDF_VERSION="v5.5.2"
IDF_TARGET="esp32s3"
IDF_REPO="https://github.com/espressif/esp-idf.git"

log() {
    printf '\n[ESP-IDF] %s\n' "$1"
}

fail() {
    printf '\n[ESP-IDF FAIL] %s\n' "$1" >&2
    exit 1
}

trap 'fail "Installation failed at line $LINENO."' ERR

# Check Git
command -v git >/dev/null 2>&1 || fail "Git is not installed."

log "Checking Git..."
git --version

# Dependencies
log "Installing dependencies..."

DEPENDENCIES=(
    git
    cmake
    ninja
    python
    python-pip
    python-virtualenv
    flex
    bison
    gperf
    ccache
    dfu-util
    libusb
    wget
    curl
    ncurses
    libffi
    openssl
)

sudo pacman -S --needed "${DEPENDENCIES[@]}"

# Directory
mkdir -p "$ESP_DIR"

# Clone
if [[ -e "$IDF_DIR" ]]; then
    fail "$IDF_DIR already exists. Move or remove it before installing."
fi

log "Cloning ESP-IDF ${IDF_VERSION}..."

log "Cloning ESP-IDF ${IDF_VERSION}..."

if [[ -z "${IDF_DIR:-}" ]]; then
    fail "IDF_DIR is empty."
fi

if [[ -d "$IDF_DIR" ]]; then
    fail "Directory already exists: $IDF_DIR"
fi

command git clone \
    --branch v5.5.2 \
    --recursive \
    https://github.com/espressif/esp-idf.git \
    "$IDF_DIR"

if [[ ! -f "$IDF_DIR/install.sh" ]]; then
    fail "ESP-IDF clone is incomplete."
fi

# Verify repository
[[ -f "$IDF_DIR/install.sh" ]] || fail "ESP-IDF installation script not found."

# Install toolchain
log "Installing ESP32-S3 toolchain..."

cd "$IDF_DIR"

./install.sh "$IDF_TARGET"

# ZSH configuration
log "Configuring ZSH..."

ZSHRC="${HOME}/.zshrc"
ALIAS="alias get_idf='source \"$IDF_DIR/export.sh\"'"

touch "$ZSHRC"

if ! grep -Fq 'alias get_idf=' "$ZSHRC"; then
    printf '\n# ESP-IDF\n%s\n' "$ALIAS" >> "$ZSHRC"
fi

log "Installation completed!"

printf '\n'
printf 'ESP-IDF: %s\n' "$IDF_DIR"
printf 'Version: %s\n' "$IDF_VERSION"
printf 'Target:  %s\n' "$IDF_TARGET"

printf '\nActivate with:\n'
printf '  source "%s/export.sh"\n' "$IDF_DIR"

printf '\nOr open a new ZSH session and run:\n'
printf '  get_idf\n'

printf '\nVerify:\n'
printf '  idf.py --version\n'
printf '  xtensa-esp32s3-elf-gcc --version\n'
