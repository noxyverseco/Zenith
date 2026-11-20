# Zenith

**A lightweight C++ duplicate file finder and manager.**

Zenith scans directories, identifies duplicate files, and helps you reclaim storage space through safe file management options.

---

## Features

- **Duplicate detection** — Groups files by size, then uses FNV-1a hashing and byte-for-byte verification.
- **Recursive scanning** — Optionally scan subdirectories.
- **Delete duplicates** — Remove duplicate files while keeping one original.
- **Move duplicates** — Move duplicates into a separate directory.
- **Dry-run mode** — Preview file operations before making changes.
- **Safety checks** — Verifies files before modifying them and avoids overwriting existing destination files.
- **Scan statistics** — Reports scanned, unreadable, and skipped files.

## Usage

```bash
dupfinder [options] <directory>
```

### Options

| Option | Description |
|:---|:---|
| `-r, --recursive` | Scan subdirectories |
| `--delete` | Delete duplicate files |
| `--move <directory>` | Move duplicates into a directory |
| `--dry-run` | Preview actions without modifying files |
| `-h, --help` | Display help |
| `-V, --version` | Display version |

### Examples

**Scan a directory**
```bash
dupfinder "C:/Users/YourName/Downloads"
```

**Scan recursively**
```bash
dupfinder -r "C:/Users/YourName/Downloads"
```

**Preview duplicate deletion**
```bash
dupfinder --delete --dry-run "C:/Users/YourName/Downloads"
```

**Move duplicates into another directory**
```bash
dupfinder --move "C:/Users/YourName/Duplicates" "C:/Users/YourName/Downloads"
```

## Build

Requires a C++17-compatible compiler.

**GCC**
```bash
g++ -std=c++17 -O2 -o dupfinder main.cpp
```

## FAQ

**It looks basic?**

Yeah, because it fucking works. Simple UI. No bullshit. Just works.

## Version

**Current version:** `1.0.0`

## License

Licensed under the [PolyForm Noncommercial License 1.0.0](https://polyformproject.org/licenses/noncommercial/1.0.0).

---

**Copyright (c) 2026 noxyverseco**

[github.com/noxyverseco/Zenith](https://github.com/noxyverseco/Zenith)
