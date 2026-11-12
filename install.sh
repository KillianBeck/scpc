#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-}"
RUN_TESTS=1

usage() {
    cat <<EOF
Usage: ./install.sh [OPTIONS]

Build, test, and install scpc.

Options:
  --prefix DIR    Install binary to DIR/bin/scpc (default: /usr/local if writable, else ~/.local)
  --skip-tests    Skip running 'make test' before installing
  -h, --help      Show this help message
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)
            if [[ $# -lt 2 ]]; then
                echo "Error: --prefix requires a directory argument" >&2
                exit 1
            fi
            PREFIX="$2"
            shift 2
            ;;
        --prefix=*)
            PREFIX="${1#*=}"
            shift
            ;;
        --skip-tests)
            RUN_TESTS=0
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Error: unknown option '$1'" >&2
            usage >&2
            exit 1
            ;;
    esac
done

if [[ -z "$PREFIX" ]]; then
    if [[ -w "/usr/local/bin" ]] || [[ "${EUID:-$(id -u)}" -eq 0 ]]; then
        PREFIX="/usr/local"
    else
        PREFIX="$HOME/.local"
    fi
fi

BIN_DIR="$PREFIX/bin"

echo "==> Building scpc..."
make -C "$SCRIPT_DIR"

if [[ "$RUN_TESTS" -eq 1 ]]; then
    echo "==> Running tests..."
    make -C "$SCRIPT_DIR" test
fi

echo "==> Installing scpc to $BIN_DIR/scpc..."
mkdir -p "$BIN_DIR"
install -m 755 "$SCRIPT_DIR/scpc" "$BIN_DIR/scpc"

ZSH_COMP_DIR="$PREFIX/share/zsh/site-functions"
BASH_COMP_DIR="$PREFIX/share/bash-completion/completions"
echo "==> Installing shell completions..."
mkdir -p "$ZSH_COMP_DIR" "$BASH_COMP_DIR"
install -m 644 "$SCRIPT_DIR/completions/_scpc" "$ZSH_COMP_DIR/_scpc"
install -m 644 "$SCRIPT_DIR/completions/scpc.bash" "$BASH_COMP_DIR/scpc"

echo "Installed scpc to $BIN_DIR/scpc"
echo "Installed zsh completion to $ZSH_COMP_DIR/_scpc"
echo "Installed bash completion to $BASH_COMP_DIR/scpc"

case ":$PATH:" in
    *":$BIN_DIR:"*) ;;
    *)
        echo "Note: $BIN_DIR is not in your PATH. Add it with:"
        echo "  export PATH=\"$BIN_DIR:\$PATH\""
        ;;
esac

echo "Note (zsh): If completions are not active, ensure '$ZSH_COMP_DIR' is in fpath before compinit in ~/.zshrc:"
echo "  fpath=($ZSH_COMP_DIR \$fpath)"
echo "  autoload -Uz compinit && compinit"

