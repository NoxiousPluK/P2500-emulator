#!/usr/bin/env python3
"""
OCR a scanned manual to text, one file per page plus a combined file.

Written for the P2219 CP/M manual, which is a scan with no text layer, so
`pdftotext` returns nothing and the whole document is invisible to search.

Two things it does that a plain `pdftoppm | tesseract` pipeline does not:

  * **Finds sideways pages.** Wide tables in these manuals are printed
    rotated. Each page is OCR'd at 0/90/270 degrees and the orientation
    with the most real words wins, so a rotated code table comes out
    readable instead of as noise.
  * **Scores every page.** The per-page word counts are printed, so a page
    that OCR'd badly is visible rather than silently empty - which matters
    when the text is going to be treated as evidence.

Needs pdftoppm (poppler) and tesseract.

usage: tools/ocr_manual.py MANUAL.pdf OUTDIR [--dpi N] [--keep-images]
"""

import re
import shutil
import subprocess
import sys
from pathlib import Path

# Words that are common in this manual and unlikely to appear in OCR noise.
WORD_RE = re.compile(r"\b[A-Za-z]{3,}\b")


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def score(text: str) -> int:
    """How much of this looks like real prose rather than speckle."""
    return len(WORD_RE.findall(text))


def ocr(png: Path, rotate: int) -> str:
    args = ["tesseract", str(png), "stdout", "--psm", "6"]
    if rotate:
        rotated = png.with_name(f"{png.stem}-r{rotate}.png")
        r = run(["magick", str(png), "-rotate", str(rotate), str(rotated)])
        if r.returncode != 0:  # ImageMagick absent: skip this orientation
            return ""
        args[1] = str(rotated)
    out = run(args)
    return out.stdout


def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) < 2:
        print(__doc__.strip().splitlines()[-1])
        raise SystemExit(1)
    pdf, outdir = Path(args[0]), Path(args[1])
    dpi = "300"
    if "--dpi" in sys.argv:
        dpi = sys.argv[sys.argv.index("--dpi") + 1]

    for tool in ("pdftoppm", "tesseract"):
        if not shutil.which(tool):
            raise SystemExit(f"{tool} not found - install poppler / tesseract")
    can_rotate = shutil.which("magick") is not None
    if not can_rotate:
        print("note: ImageMagick not found, sideways pages will not be detected")

    outdir.mkdir(parents=True, exist_ok=True)
    images = outdir / "pages"
    images.mkdir(exist_ok=True)
    print(f"rendering {pdf.name} at {dpi} dpi ...")
    run(["pdftoppm", "-r", dpi, "-png", str(pdf), str(images / "p")])

    pages = sorted(images.glob("p-*.png"))
    if not pages:
        raise SystemExit("pdftoppm produced no pages")

    combined = []
    for png in pages:
        best, best_rot, best_score = "", 0, -1
        for rot in ((0, 90, 270) if can_rotate else (0,)):
            text = ocr(png, rot)
            sc = score(text)
            if sc > best_score:
                best, best_rot, best_score = text, rot, sc
        n = png.stem.split("-")[-1]
        (outdir / f"page-{n}.txt").write_text(best, encoding="utf-8")
        flag = f"  ROTATED {best_rot}deg" if best_rot else ""
        print(f"  page {n}: {best_score} words{flag}")
        combined.append(f"\n\n===== PAGE {n} ====={flag}\n\n{best}")

    (outdir / "manual.txt").write_text("".join(combined), encoding="utf-8")
    print(f"\nwrote {len(pages)} pages to {outdir}/ (combined: manual.txt)")
    if "--keep-images" not in sys.argv:
        shutil.rmtree(images, ignore_errors=True)


if __name__ == "__main__":
    main()
