# Agent notes

## Viewing PNG screenshots in the Preview panel

The Preview panel does not load PNG files that are referenced by path. To look at screenshots:

1. Write a single HTML file inside the project (for example `.debug_shots/index.html`).
2. Embed each PNG as a base64 data URI: `<img src="data:image/png;base64,...">`. Generate it with Python, reading the bytes and calling `base64.b64encode`.
3. Open it with `register_preview` using `htmlPath`, then call `preview_screenshot`.

Delete the debug folder when you are done with it.
