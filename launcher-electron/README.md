# Electron launcher development slice

This is an incomplete development build. It checks the standard launcher roots on startup and also lets the player select a launcher root manually; it shows a basic installation status. It can invoke the selected installed launcher’s existing Play entry point; installation, updates, complete running-game detection, controller navigation and packaging are not implemented. This integration has only been tested with fake processes, not a real game.

Use Node.js and npm and a Python environment able to import the existing launcher modules. From this directory, run `npm ci` followed by `npm start`. The Electron package downloads its runtime during installation. Set `NX2SARIA_PYTHON` to the Python executable when it is not on PATH. When no standard installation is detected, select the existing launcher root containing `data/launcher.json`, not the game folder. The Play button starts the installed Python and launcher version, which may open the existing launcher window for flows it cannot handle headlessly. A job marker prevents Play, even if stale. Do not run this development build as a replacement for the existing launcher.

`npm test` builds TypeScript and checks the inspection boundary using a temporary folder. `python -m unittest discover -s tests -p electron_inspection_test.py` from the repository root tests the Python inspection command.

On Linux, `xvfb-run -a npm run test:ui` exercises the actual Electron window with a native-dialog stub, temporary installation and fake executable. It does not run the game.

On Linux the window hides only after the game executable is observed below the delegated launcher process and the saved preference allows it; it is restored when that process ends. This has been exercised with synthetic processes only. On Windows, automatic hiding uses a read-only process snapshot and is covered by synthetic process tests, but has not yet been validated with the real game or window.
