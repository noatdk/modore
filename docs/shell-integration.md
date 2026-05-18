# Shell-Native Conversion

Shell-native conversion keeps the shell in charge of its own line editor. The
shell reads the current buffer, calls `modore-host`, then writes the result
back itself.

Helper:

```sh
modore-host --shell-convert --caret <utf16-cursor>
```

It reads a buffer from stdin, converts the token at the caret, and prints the
updated line.

Load the bootstrap for your shell:

```sh
# bash or zsh
source scripts/modore-shell-bootstrap.sh

# fish
source scripts/modore-shell-bootstrap.fish
```

The loader is lazy. Sourcing it only installs a one-shot prompt hook; the first
prompt asks `modore-host` for the shell-specific binding snippet.

The shell binding talks to the already-running macOS host over a local Unix
socket. `modore-host --shell-convert` is just the client shim.

## Notes

- The generated snippet uses the configured Modore hotkey when it can be
  represented cleanly in the active shell.
- If it cannot, the bootstrap falls back to a conventional shell chord.
- `Ctrl-X Ctrl-L` opens the shell candidate chooser. It uses `gum`.
