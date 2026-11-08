# OverTone website

Static HTML, CSS and JavaScript. No build step or package manager is needed.
Edit the website in this directory on main; gh-pages is the publishing copy.

## Preview

From the repository root, run python3 -m http.server --bind 127.0.0.1 8080
and open http://127.0.0.1:8080/site/.

Download files and checksums are in assets/downloads/. Keep each checksum and the
installation instructions aligned with its file. Source ZIPs exclude the website
apart from the image referenced by the project README.

## GitHub Pages

After committing website changes on main, prepare its publishing branch:

~~~sh
git subtree split --prefix=site --branch=gh-pages main
git push origin main gh-pages
~~~

In repository Settings → Pages, use Deploy from a branch, gh-pages and /(root).
The .nojekyll file serves these static files without a Jekyll build.
Changing the branch alone does not enable Pages. Once Pages is enabled, pushes
to gh-pages deploy the website; review its contents before pushing.
A Pages site can be public even when its source repository is private.

The project site address is https://overcesare.github.io/OverTone/.
Verify links and downloads under that path, including the trailing slash.

The trailer uses the official YouTube privacy-enhanced iframe, created only when
the visitor selects its local cover button. Keep the noscript YouTube link as a fallback
and keep privacy.html aligned with third-party connections. Walkthroughs remain
local video files. No YouTube SDK or API key is required.

CSS and JavaScript URLs include the first 12 characters of their SHA-256 in a
v query parameter. Update those references when the file changes so a new page
cannot reuse an incompatible script or stylesheet from a previous deployment.
