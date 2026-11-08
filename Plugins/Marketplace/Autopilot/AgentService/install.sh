#!/bin/sh
# Installs Autopilot's agent service (the `autopilot` command) from the wheel beside this script, on macOS or Linux.
#
#   sh install.sh
#
# It uses uv (https://docs.astral.sh/uv/), installing it first if it isn't there, and uv brings a Python 3.12 of its
# own; the agent, OpenCode, runs under Bun (https://bun.sh), installed the same way: nothing else needs to be
# installed. Run it again to update. Uninstall with `autopilot uninstall` (or Remove from this computer, on a
# plugin's Settings page).
set -eu

here=$(cd "$(dirname "$0")" && pwd)
wheel=$(ls "$here"/autopilot-*.whl 2>/dev/null | head -n 1)
if [ -z "$wheel" ]; then
  echo "No autopilot-*.whl next to install.sh: unpack the whole download, then run it again." >&2
  exit 1
fi

if ! command -v uv >/dev/null 2>&1; then
  echo "Installing uv (https://docs.astral.sh/uv/), which installs Autopilot and its Python..."
  curl -LsSf https://astral.sh/uv/install.sh | sh
  PATH="$HOME/.local/bin:$PATH"
fi

echo "Installing Autopilot from $(basename "$wheel")..."
# The wheel is installed from a copy dated now, in a folder of its own. uv keeps a local wheel it has unpacked by its
# path and date, and every download's files are dated alike (2026-01-01, tools/package_release.py), so a plugin updated
# in place would have uv install the build before it again, from its cache, and say it installed this one
fresh=$(mktemp -d "${TMPDIR:-/tmp}/autopilot-wheel-XXXXXX")
trap 'rm -rf "$fresh"' EXIT
cp "$wheel" "$fresh/"
touch "$fresh/$(basename "$wheel")"
uv tool install --force --python 3.12 "autopilot[claude,codex] @ file://$fresh/$(basename "$wheel")"
uv tool update-shell >/dev/null 2>&1 || true

case "$(uname -s)" in
  Darwin) base="$HOME/Library/Application Support" ;;
  *) base="${XDG_CONFIG_HOME:-$HOME/.config}" ;;
esac
settings="$base/Autopilot"
env_file="$settings/autopilot.env"

# Which agent service this is (agent-service.json beside this script), kept in the settings folder: a plugin carrying
# another one installs its own before it starts the agent service
if [ -f "$here/agent-service.json" ]; then
  mkdir -p "$settings"
  cp "$here/agent-service.json" "$settings/agent-service.json"
fi

# Settings the download carries (settings.env beside this script, e.g. which Aura server it was built for) go into
# autopilot.env over what is there; those an earlier download carried and this one doesn't are taken out
# (python -I, here and below: the installed Autopilot, whatever folder this runs in; from one holding a checkout's
# autopilot/, that was imported instead, and OpenCode's packages went into it)
python="$(uv tool dir)/autopilot/bin/python"
"$python" -I -c "import sys; from autopilot import account; account.apply_download_settings(sys.argv[1])" "$here/settings.env"

# The Coding Agents: Claude Code and Codex come with Autopilot; OpenCode, the default, runs under Bun (installed here when it
# isn't there), and its packages are fetched now, not at the first run. Without Bun, Claude Code is the default
if ! command -v bun >/dev/null 2>&1 && [ ! -x "$HOME/.bun/bin/bun" ]; then
  echo "Installing Bun (https://bun.sh), which runs the agent..."
  curl -fsSL https://bun.sh/install | bash || true
fi
if ! "$python" -I -c "from autopilot.agents import opencode; opencode.ensure_installed(opencode.bun())"
then
  mkdir -p "$settings"
  if ! grep -Eq '^[[:space:]]*(export[[:space:]]+)?AUTOPILOT_AGENT[[:space:]]*=' "$env_file" 2>/dev/null; then
    echo "AUTOPILOT_AGENT=claude" >> "$env_file"
    chmod 600 "$env_file"
  fi
  echo "OpenCode couldn't be set up under Bun, so Autopilot will use Claude Code; Codex works too (Coding Agent in the"
  echo "first-run setup). Install Bun (https://bun.sh), then run this again to use OpenCode."
fi

cat <<'EOF'

Autopilot is installed. Autopilot starts it with your editor: open the Autopilot tab and finish its first-run
setup. To run it yourself instead, run this before you open the editor (Autopilot connects to it):
  autopilot serve
EOF
