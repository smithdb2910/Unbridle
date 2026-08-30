# Unbridle Linux

A lightweight Linux utility for managing Discord network/proxy configuration.

This project came from a hurdle I ran into while getting Discord working correctly on Linux. I worked through the problem and built this as the result.

## Features

* Simple graphical interface
* Proxy/network configuration
* Direct connection mode
* Reset option for situations where the network configuration becomes stuck, mixed up, or otherwise doesn't behave correctly
* Lightweight native Linux implementation

## Requirements

* Linux
* GCC / build tools
* GTK3 development libraries

On Debian/Ubuntu:

```bash
sudo apt update
sudo apt install build-essential pkg-config libgtk-3-dev
```

## Build

Clone the repository and enter the project directory:

```bash
git clone https://github.com/YOUR-USERNAME/unbridle-linux.git
cd unbridle-linux
```

Build with:

```bash
make
```

## Run

After building:

```bash
./unbridle
```

If necessary, make the executable runnable:

```bash
chmod +x unbridle
./unbridle
```

## Reset

If the network or proxy configuration gets into an unexpected state, use the **Reset** button in the application.

Reset returns the configuration to the direct/default state so you can start again cleanly.

If Discord was already running, restart Discord after performing a reset.

## Notes

The project is intentionally kept simple. The goal is to provide a straightforward way to configure the required network settings without having to manually work through the configuration each time.

## License

MIT License
