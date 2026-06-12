# termfun

Terminal eye-candy: fireworks and graphics demos using [termpaint](https://github.com/termpaint/termpaint).

## Build

```sh
git clone --recurse-submodules https://github.com/YOUR_USER/termfun
cd termfun
make
```

If you already cloned without `--recurse-submodules`, initialize the submodule first:

```sh
git submodule update --init
make
```

## Run

```sh
make run        # fireworks TUI
make run-gfx    # fireworks with kitty graphics protocol
./build/kitty_probe  # detect kitty graphics support
```
