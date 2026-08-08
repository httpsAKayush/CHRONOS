#!/usr/bin/env bash
set -e

echo "[+] Starting Chronos Installation..."

# Build Chronos
echo "[+] Building Chronos..."
cd "$(dirname "$0")"
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

echo "[+] Attempting global installation (/usr/local/bin)..."
if sudo -n make install 2>/dev/null; then
    echo "[✔] Successfully installed chronos globally to /usr/local/bin"
else
    echo "[!] Sudo privileges not available or denied."
    echo "[+] Falling back to user-local installation (~/.local/bin)..."
    
    mkdir -p ~/.local/bin
    cp chronos chronos-daemon chronos-indexer ~/.local/bin/
    
    echo "[✔] Successfully installed chronos to ~/.local/bin"
    
    # Check if ~/.local/bin is in PATH
    if [[ ":$PATH:" != *":$HOME/.local/bin:"* ]]; then
        echo "[!] ~/.local/bin is not in your PATH."
        
        # Add to bashrc or zshrc
        SHELL_RC="$HOME/.bashrc"
        if [ -n "$ZSH_VERSION" ] || [[ "$SHELL" == *"zsh"* ]]; then
            SHELL_RC="$HOME/.zshrc"
        fi
        
        echo "" >> "$SHELL_RC"
        echo 'export PATH="$HOME/.local/bin:$PATH"' >> "$SHELL_RC"
        echo "[+] Added ~/.local/bin to $SHELL_RC"
        echo "[+] Please run: source $SHELL_RC or restart your terminal."
    fi
fi

echo "[✔] Installation complete! You can now run 'chronos' from anywhere."
