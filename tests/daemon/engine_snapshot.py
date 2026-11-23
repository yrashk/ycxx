"""Reuse a copied phase 1/2 snapshot in fresh compiler processes."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile


def main(ycxx):
    with tempfile.TemporaryDirectory(prefix="ycxx-snapshot-") as temp:
        root = Path(temp)
        producer, consumer, original, restored, paging = (
            root / name for name in ("producer", "consumer", "original", "restored", "paging")
        )
        for directory in (producer, consumer, original, paging):
            directory.mkdir()
        padding = b"// " + b"a" * 5000 + b"\r\n"
        (producer / "main.cpp").write_bytes(b"\xef\xbb\xbf" + padding + b"#define V 4\\\r\n2\r\nint answer = V;\r\n")
        shutil.copyfile(producer / "main.cpp", consumer / "main.cpp")

        env = os.environ.copy()
        for variable in ("YCXX_ENGINE_SNAPSHOT_DIR", "YCXX_ENGINE_PAGING_DIR"):
            env.pop(variable, None)

        def preprocess(directory, **changes):
            return subprocess.run(
                [ycxx, "cxx", "--no-daemon", "-E", "main.cpp"], cwd=directory,
                env=env | changes, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True,
            ).stdout

        def entries(directory):
            return {entry.name: entry.stat().st_ino for entry in directory.glob("*.source")}

        want = preprocess(producer)
        assert b"int answer = 42;" in want, want
        assert preprocess(producer, YCXX_ENGINE_SNAPSHOT_DIR=str(original)) == want
        assert entries(original), "compiler did not publish snapshots"

        shutil.copytree(original, restored)
        before = entries(restored)
        wanted_consumer = preprocess(consumer)
        assert preprocess(consumer, YCXX_ENGINE_SNAPSHOT_DIR=str(restored)) == wanted_consumer
        assert entries(restored) == before, "a warm consumer replaced an artifact"

        (consumer / "main.cpp").write_bytes(padding + b"int answer = 17;\r\n")
        assert b"int answer = 17;" in preprocess(consumer, YCXX_ENGINE_SNAPSHOT_DIR=str(restored))
        assert before.items() <= entries(restored).items()
        assert entries(restored).keys() > before.keys()

        for name in before:
            bad = restored / name
            bad.unlink()
            bad.write_bytes(b"truncated")
        (consumer / "main.cpp").write_bytes((producer / "main.cpp").read_bytes())
        assert preprocess(consumer, YCXX_ENGINE_SNAPSHOT_DIR=str(restored)) == wanted_consumer
        assert all((restored / name).stat().st_size > 9 for name in before)
        assert preprocess(consumer, YCXX_ENGINE_PAGING_DIR=str(paging)) == wanted_consumer
        assert not list(paging.iterdir()), "paging left named temporary storage"
        assert preprocess(consumer, YCXX_ENGINE_PAGING_DIR=str(root / "missing")) == wanted_consumer


if __name__ == "__main__":
    main(str(Path(sys.argv[1]).resolve()))
