# Unbridle (Linux, minimal)

A small LD_PRELOAD hook that makes Discord's Linux client route through a
proxy and/or perform a light UDP manipulation that helps bypass regional
voice-chat blocking (e.g. UAE) - without a VPN, and without touching any
other application's network traffic. Linux port of
[hunbridle/unbridle](https://github.com/hunbridle/unbridle).

## Build

```
make
```

Needs GTK3 dev headers (`libgtk-3-dev` on Debian/Ubuntu/Fedora-equivalent).

## Use

Run `build/unbridle`. Direct Mode is preselected (recommended for
most regional voice-blocking cases - no proxy, just the UDP manipulation).
Click **Activate**, then launch Discord the way you always do - no need to
find a different icon. Click **Deactivate** to undo it.

Discord must be fully closed when you click Activate/Deactivate.

## How activation works

Activate patches the actual Discord `.desktop` launcher (backing up any
existing one first) by writing a same-named override to
`~/.local/share/applications/`, which Linux desktops treat as higher
priority than the system copy. This is entirely user-level - no root
needed, and nothing outside your own Discord shortcut is touched.

## Supported installs

Native Discord only (official `.deb`/`.rpm`, or the `.tar.gz` extracted to
your home directory). **Flatpak and Snap will not work** - both sandbox
the process in a way that blocks LD_PRELOAD from reaching it. This is a
sandboxing limitation, not a Discord-version limitation; there's no
dependency on which Discord release you're running.

## `unbridle-packet.bin` (optional, off by default)

If you create `~/.config/unbridle/unbridle-packet.bin`, its contents
get sent once before Discord's real voice packet, in addition to the
built-in manipulation. It's re-read every time, so you can edit it live.
Nothing is sent here unless you put something there yourself - there is no
bundled default packet.

## If voice still doesn't work after activating

Confirm the hook actually loaded into every Discord-related process
(Discord's Linux client spawns several):

```
for pid in $(pgrep -f -i discord); do
  echo "$pid: $(tr '\0' '\n' < /proc/$pid/environ 2>/dev/null | grep -c LD_PRELOAD)"
done
```

If every line ends in `1`, the hook is loaded everywhere and any remaining
issue is about the manipulation payload itself, not delivery. If some show
`0`, that process never got the hook - worth reporting back with which one.
