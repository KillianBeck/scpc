# scpc

A fast C wrapper for `scp` featuring:

- Real-time terminal progress bar
- Average transfer speed
- Estimated remaining time (ETA)
- Recursive directory scanning (`-r`) for accurate total size estimation
- Parallel file transfers via `-w` / `--workers`, or automatic worker count selection when omitted
- Full passthrough of standard `scp` flags (`-346ABCOpqRrsTv`, `-c`, `-D`, `-F`, `-i`, `-J`, `-l`, `-o`, `-P`, `-S`, `-X`)

## Install

Run the install script to build, test, and install `scpc`:

```sh
./install.sh
```

By default, it installs to `/usr/local/bin/scpc` if writable, or `~/.local/bin/scpc` otherwise. You can also specify a custom prefix:

```sh
./install.sh --prefix /usr/local
```

## Build & Test Manually

```sh
make
make test
```

## Tab Completions (Hosts & Options)

`install.sh` installs Zsh (`completions/_scpc`) and Bash (`completions/scpc.bash`) completions that complete:

- Hostnames from `~/.ssh/config`, `~/.ssh/known_hosts`, and `/etc/hosts` (including `user@host:` prefixes)
- Remote paths after `host:` (via non-interactive `ssh`)
- Local files and all `scpc` / `scp` flags

For Zsh, if your prefix is not already in `fpath`, add to `~/.zshrc`:

```zsh
fpath=(~/.local/share/zsh/site-functions $fpath)
autoload -Uz compinit && compinit
```

For Bash, source `completions/scpc.bash` (or install `bash-completion`):

```bash
source ~/.local/share/bash-completion/completions/scpc
```

## Usage

```sh
# Auto-determine worker count and recursively scan directory size
scpc -r ./local_dir user@host:/remote/dest/

# Manually specify 4 parallel workers
scpc -r -w 4 ./local_dir user@host:/remote/dest/

# Standard scp options are forwarded
scpc -P 2222 -i ~/.ssh/id_ed25519 -C -r ./data user@host:/backup/
```
