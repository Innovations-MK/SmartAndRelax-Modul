Import("env")
import gzip
import os
import pathlib
import shutil
import stat
import time

# SmartAndRelax LittleFS-GZIP-Build – Windows/OneDrive robust.
# data/    = echte Quelldateien
# datazip/ = nur generierter Build-Arbeitsordner
#
# WICHTIG: datazip wird absichtlich NICHT mehr als Ordner geloescht.
# OneDrive/Explorer kann den Ordner selbst kurz sperren (WinError 5).
# Stattdessen werden nur seine Inhalte bereinigt und der Ordner bleibt bestehen.


def _make_writable(path):
    try:
        os.chmod(path, stat.S_IWRITE | stat.S_IREAD)
    except OSError:
        pass


def _remove_path(path):
    path = pathlib.Path(path)
    if not path.exists() and not path.is_symlink():
        return
    _make_writable(path)
    if path.is_dir() and not path.is_symlink():
        def onerror(func, p, exc_info):
            _make_writable(p)
            func(p)
        shutil.rmtree(path, onerror=onerror)
    else:
        path.unlink()


def clear_datazip_contents(data_dir):
    data_dir = pathlib.Path(data_dir)
    data_dir.mkdir(parents=True, exist_ok=True)

    last_error = None
    for attempt in range(1, 8):
        try:
            for child in list(data_dir.iterdir()):
                _remove_path(child)
            # Root remains on purpose. Verify it is empty.
            if not any(data_dir.iterdir()):
                return
        except OSError as exc:
            last_error = exc
        time.sleep(0.20 * attempt)

    remaining = []
    try:
        remaining = [p.name for p in data_dir.iterdir()]
    except OSError:
        pass
    raise RuntimeError(
        "datazip konnte nicht geleert werden. Der Ordner selbst wird nicht geloescht. "
        f"Verbleibend: {remaining}. Letzter Fehler: {last_error}"
    )


def copy_data(src, dst):
    ext = pathlib.Path(src).suffix[1:].lower()
    myfilename = "filelist.txt"
    dst_path = pathlib.Path(dst)
    dst_path.parent.mkdir(parents=True, exist_ok=True)

    with open(dst_path.parent / myfilename, "a", encoding="utf-8", newline="\n") as myfile:
        if ext in ["js", "css", "html", "ico"]:
            myfile.write(dst_path.name + ".gz\n")
            with open(src, "rb") as src_file, gzip.open(str(dst_path) + ".gz", "wb") as dst_file:
                for chunk in iter(lambda: src_file.read(4096), b""):
                    dst_file.write(chunk)
        else:
            myfile.write(dst_path.name + "\n")
            shutil.copy2(src, dst)


def del_gzip_data(source, target, env):
    print("Clearing zipped files (contents only; keeping datazip root for OneDrive)")
    clear_datazip_contents(env.get("PROJECT_DATA_DIR"))


def copy_gzip_data(source, target, env):
    data = pathlib.Path(env.get("PROJECT_DATA_DIR"))
    source_data = pathlib.Path(str(data)[:-3])  # datazip -> data; existing project layout

    if not source_data.is_dir():
        raise RuntimeError(f"LittleFS source folder not found: {source_data}")

    clear_datazip_contents(data)
    print("zipping html files")
    shutil.copytree(source_data, data, dirs_exist_ok=True, copy_function=copy_data)


env.AddPreAction("$BUILD_DIR/littlefs.bin", copy_gzip_data)
env.AddPostAction("$BUILD_DIR/littlefs.bin", del_gzip_data)
