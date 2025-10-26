# Letterbox - Word Study Tool

A Qt6-based word study application that helps you learn Scrabble anagrams with hooks and extensions using the MAGPIE library.

## Features

- **Alphagram-based study**: Words grouped by vowels-first alphagrams (e.g., AABDRV for BRAVAD)
- **Priority ordering**: Alphagrams sorted by utility value (most important words first)
- **Hook visualization**:
  - Front hooks (letters that can be added to the start) shown in medium font
  - Back hooks (letters that can be added to the end) shown in medium font
  - Word itself shown in large bold font
- **Progress tracking**: Status bar shows your study progress through the word list
- **Clean interface**: Simple, distraction-free design matching the reference screenshot

## Building

```bash
# 1. Build MAGPIE library first (from parent directory)
cd ../
make libmagpie.a

# 2. Build Letterbox
cd letterbox
qmake Letterbox.pro
make

# 3. Run the application
open Letterbox.app          # macOS
# or
./Letterbox                 # Linux
```

## Data Setup

The application requires:
- `data/` symlink pointing to `../data` (contains KWG lexicon files and letter distributions)
- `csw24-pb.txt` containing utility values and words (format: `<utility> <word>` per line)

Both should already be set up if you followed the build steps above.

## Usage

1. Launch the application
2. Browse through alphagram sets using the "Next" button
3. Mark sets as studied using the "Mark Studied" button
4. Track your progress with the status bar at the bottom

## Architecture

- **MAGPIE wrapper** (`magpie_wrapper.c/h`): C interface to MAGPIE's word search and hook finding functions
  - Uses DAWG root for anagram generation
  - Uses GADDAG for front hook finding
  - Uses DAWG for back hook finding

- **Qt UI** (`letterbox_window.cpp/h`):
  - Loads and parses csw24-pb.txt
  - Groups words by vowels-first alphagrams
  - Displays words with different font sizes for word/hooks/extensions
  - Tracks study progress

## Vowels-First Alphagram Sorting

Unlike traditional alphabetical alphagrams (e.g., AABDRV), this tool sorts vowels first for easier pattern recognition:
- Vowels (AEIOU) come first, sorted alphabetically
- Consonants follow, sorted alphabetically
- Example: BRAVADO → AAOBDRV (AA-O first, then BDRV)

## Lexicon

Currently configured for CSW24 (Collins Scrabble Words 2024). You can change the lexicon in `letterbox_window.cpp` by modifying the `letterbox_create_config()` call.
