Web UI conventions:

- Edit the homepage in `src/root.html`.
- `src/root.html.h.py` generates `src/root.html.h` at build time.
- `src/main.cpp` includes `root.html.h` directly.
- The homepage JS fetches `/raw/last` and `/raw/all`.
- The graph must use sample timestamps directly and handle non-even intervals.
- Keep browser/debug headers limited to the raw routes used by the webpage.
- Do not add those extra headers to `/last` unless there is a specific need.
