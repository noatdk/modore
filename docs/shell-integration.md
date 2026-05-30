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

## Keys

| Chord           | Action                                   |
| --------------- | ---------------------------------------- |
| `Ctrl-X Ctrl-J` | Convert / cycle the token at the caret   |
| `Ctrl-X Ctrl-K` | Convert as katakana / cycle backward     |
| `Ctrl-X Ctrl-L` | Open the candidate chooser               |

Chords are fixed (`Ctrl-X` prefix); they do not track the GUI hotkey.

## Candidate window

On zsh, cycling shows the candidate list below the prompt (ZLE `POSTDISPLAY`,
current pick in brackets). No dependency. It clears on the next keystroke or
when the line is accepted. bash and fish cycle without the inline list —
readline and fish expose no equivalent below-buffer region. Turn it off with
`[shell] candidate_window = off`.

`Ctrl-X Ctrl-L` opens a full chooser. By default the picker is auto-detected:

1. `fzf` (preferred — widest coverage, non-fullscreen)
2. `gum`
3. a built-in numbered prompt (no dependency; type the number, Enter)

Pin one with `[shell] picker = fzf|gum|numeric` in `modore.conf`, or override
per shell with `MODORE_SHELL_PICKER=fzf|gum|numeric` (env wins over config).
Both knobs are in [configuration.md](configuration.md); they take effect in
shells started after the change.

## Notes

- The per-keystroke widgets call a lean `modore-shell` client that relays the
  request to the running host over a Unix socket. It links nothing from the
  engine, so each keypress is a ~ms process spawn rather than a 25 MB dylib
  load. The host bakes the socket path into the snippet (exported as
  `MODORE_SHELL_SOCKET`), so the client never has to guess where the daemon
  listens.
- The one-time bootstrap sourced at shell start still calls
  `modore-host --print-shell-bootstrap` to generate the snippet.
- Linux has no shell server yet, so shell-native conversion is macOS-only.
