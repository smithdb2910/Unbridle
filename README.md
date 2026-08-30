# Unbridle

**Unbridle** is a lightweight Linux application designed to improve Discord voice connectivity.

Unbridle handles Discord's network traffic separately, allowing Discord to use an alternate connection path while the rest of your applications continue using your normal internet connection.

## Features

* Designed for Discord
* Native Linux application
* Lightweight and simple
* Only affects Discord traffic
* Does not interfere with other applications
* No complicated configuration required
* Easy to build from source
* Open source and free to use

## Building

Clone the repository and enter the project directory:

```bash
cd Unbridle
```

Build the project:

```bash
make
```

The compiled files will be placed in:

```text
build/unbridle
build/libunbridle.so
```

## Running

After building, start Unbridle with:

```bash
./build/unbridle
```

To see the available options:

```bash
./build/unbridle --help
```

## What Unbridle Does

Unbridle is designed for situations where Discord voice connectivity is unavailable, unreliable, or unnecessarily slow.

It focuses specifically on Discord rather than changing how your entire computer connects to the internet.

Your browser, games, downloads, and other applications continue using your normal connection.

### Example

Without Unbridle:

```text
Computer
│
├── Discord
├── Browser
├── Games
└── Other applications
```

With Unbridle:

```text
Computer
│
├── Unbridle
│   └── Discord
│
├── Browser
├── Games
└── Other applications
```

This keeps Unbridle focused on the application it was designed for.

## Linux Support

Unbridle is developed for Linux.

Fedora Linux is currently a primary development environment. Other Linux distributions may work if the required dependencies are available.

## Contributing

Unbridle is free and open source.

Contributions are welcome. You can:

* Add new features
* Improve existing functionality
* Fix bugs
* Improve compatibility
* Adjust the project for other Linux distributions
* Fork the project and build your own version

If you make improvements that could benefit other users, consider opening a pull request.

## License

Unbridle is free and open-source software released under the [MIT License](LICENSE).

You are free to use, modify, copy, distribute, and build upon the project in accordance with the license.

## Disclaimer

Unbridle is provided as-is, without warranty.

Users are responsible for complying with the laws, regulations, network policies, and terms of service applicable to their location and network.
