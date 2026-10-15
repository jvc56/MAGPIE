# Checking MAGPIE on Windows: a checklist for volunteers

Thank you for trying MAGPIE on Windows. Automated tests already run it on Windows, but
they can't double-click a file, see what Windows warns about, or judge how the console
looks. This checklist covers those things. It takes about 20 minutes.

You need Windows 10 (version 1903 or later) or Windows 11, on an ordinary PC (x64) or a
Windows on ARM laptop (for example, a Snapdragon one), and an internet connection.

For each item, note **OK** or what went wrong. A screenshot helps whenever something
looks off. Send your notes, with your Windows version (Settings, System, About) and
which `.exe` you used, to the person who asked you to test.

## 1. Download and first launch

1. From the release page you were given, download the `.exe` for your PC:
   - `magpie-<version>-windows-x64.exe` for most PCs;
   - `magpie-<version>-windows-arm64.exe` for Windows on ARM.

   Put it in a new, empty folder, such as `Documents\MAGPIE`.
2. Did your browser or antivirus warn about the download, block it, or delete it? If so,
   what exactly did it say?
3. Optional: check the file is the one published. Open PowerShell in that folder and run:
   ```
   Get-FileHash .\magpie-<version>-windows-x64.exe -Algorithm SHA256
   ```
   Does the hash match the one in the `.sha256` file on the release page?
4. Double-click the `.exe`.
   - Windows will probably show **Windows protected your PC**. Click **More info**, then
     **Run anyway**. Was the wording what you expected? Please take a screenshot of this
     window: it will go in the README.
   - A console window should open with a line starting `MAGPIE` and the version.

## 2. Getting the data

1. Because the folder is empty, MAGPIE should say its data files are missing and offer
   to download them. Answer `y`.
2. Did it show progress for five parts and then say the data is ready? How long did it
   take, and roughly how fast is your connection?
3. Does a `data` folder now exist beside the `.exe`?
4. Close the window (type `quit`), then double-click the `.exe` again. This time it
   should not ask to download anything.

## 3. Everyday use

Type each of these at the `magpie>` prompt and note anything odd.

1. `set -lex CSW24`. The first time, it should say it is building a wordmap, then finish
   within a minute.
2. `cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AEINRST/ 0/0 0`
   - Does the board draw with clean lines and columns, no stray symbols such as `←[0m`,
     `â”` or empty boxes? In the plain console window the column letters are `A B C … O`
     and the turn marker is `>`. Windows Terminal shows wider letters and `➤`.
   - Are the premium squares coloured?
3. `gen`: a list of moves should appear.
4. `sim -plies 2 -it 1000`: it should finish and show win percentages.
5. Arrow keys: press up-arrow at the prompt. Does it bring back your previous command?
   Can you edit it with left and right arrows before pressing Enter?
6. While a long command runs, type `stop` and press Enter. For example, run
   `autoplay games 100000` first, then type `stop`. Does it stop?
7. `help`: does the help text display properly?

## 4. Errors stay readable

1. Rename the `data` folder to `data-hidden`, then double-click the `.exe`.
2. When it offers to download the data, answer `n`. MAGPIE can't start without its data,
   so it should print why, then **Press Enter to close this window.** Does the window
   stay open until you press Enter, so the message can be read?
3. Rename the folder back to `data`.

## 5. Running from a terminal

1. Open PowerShell in the folder: Shift + right-click in the folder, then **Open
   PowerShell window here**, or open **Terminal** and use `cd`.
2. Run:
   ```
   .\magpie-<version>-windows-x64.exe cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AEINRST/ 0/0 0 -lex CSW24
   ```
   It should show the empty board and wait at the `magpie>` prompt. Type `gen` and press
   Enter: does it print moves? Type `quit` to leave.
3. If you have both, try it in **Windows Terminal** and in the old **Command Prompt**
   (`cmd.exe`). Does the board look right in both?

## 6. Unusual names

1. If your Windows user name contains accented or non-Latin letters (for example
   `José` or `Łukasz`), the test above already covered it. Say so in your notes.
2. Otherwise, make a folder with such a name, for example `Documents\Ünïcödé`, copy the
   `.exe` and the `data` folder into it, and repeat section 3, steps 1 to 3.

## 7. Anything else

Did anything surprise you? Did Windows or your antivirus react at any point after the
first launch? Is anything slow that you expected to be fast?
