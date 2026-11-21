"""Archive a FAT-compatible directory tree using an optional reproducible date."""
from datetime import datetime, timezone
import os
from pathlib import Path
import sys
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo


def main(tree, output):
    # The ZIP format starts in 1980. Default to the upstream release date.
    stamp = max(315532800, int(os.environ.get("SOURCE_DATE_EPOCH", "1768348800")))
    processed = datetime.fromtimestamp(stamp, timezone.utc).timetuple()[:6]
    with ZipFile(output, "w", compression=ZIP_DEFLATED) as bundle:
        for file in sorted(tree.rglob("*")):
            if file.is_file():
                entry = ZipInfo(file.relative_to(tree).as_posix(), processed)
                entry.external_attr = 0o644 << 16
                bundle.writestr(entry, file.read_bytes(), compress_type=ZIP_DEFLATED)
    with ZipFile(output) as bundle:
        if bundle.testzip() is not None:
            raise ValueError("Could not verify the EFI archive")


if __name__ == "__main__":
    main(Path(sys.argv[1]), Path(sys.argv[2]))
