"""ctypes wrapper and interactive REPL for the magpie shared library.

Build the library first with `make libmagpie`, then run this script from
the MAGPIE repo root:

    python3 examples/magpie.py

Every returned string is copied to a Python str and the original C string
is released with magpie_free_string, so no magpie allocations leak into
the Python side.
"""

import ctypes
import os
import platform
import sys

MAGPIE_SUCCESS = 0
MAGPIE_ERROR = 1
MAGPIE_DID_NOT_RUN = 2

THREAD_STATUS_UNINITIALIZED = 0
THREAD_STATUS_STARTED = 1
THREAD_STATUS_USER_INTERRUPT = 2
THREAD_STATUS_FINISHED = 3


class MagpieError(Exception):
    """Raised when a magpie command fails."""


class Magpie:
    def __init__(self, data_paths="./data", lib_path=None):
        if lib_path is None:
            ext = "dylib" if platform.system() == "Darwin" else "so"
            lib_path = os.path.join("bin", f"libmagpie.{ext}")
        self._lib = ctypes.CDLL(lib_path)
        self._declare_functions()
        self._mp = self._lib.magpie_create(data_paths.encode("utf-8"))
        if self._lib.magpie_has_error(self._mp):
            error = self._take_string(self._lib.magpie_get_and_clear_error(self._mp))
            self.close()
            raise MagpieError(f"failed to create magpie: {error}")

    def _declare_functions(self):
        lib = self._lib
        # Returned strings are declared as c_void_p rather than c_char_p so
        # the original pointer can be passed back to magpie_free_string.
        lib.magpie_create.restype = ctypes.c_void_p
        lib.magpie_create.argtypes = [ctypes.c_char_p]
        lib.magpie_create_with_options.restype = ctypes.c_void_p
        lib.magpie_create_with_options.argtypes = [
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_bool,
        ]
        lib.magpie_destroy.restype = None
        lib.magpie_destroy.argtypes = [ctypes.c_void_p]
        for func in (
            lib.magpie_run_sync,
            lib.magpie_run_sync_human_readable,
            lib.magpie_run_async,
            lib.magpie_run_async_human_readable,
        ):
            func.restype = ctypes.c_int
            func.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        lib.magpie_await.restype = ctypes.c_int
        lib.magpie_await.argtypes = [ctypes.c_void_p]
        lib.magpie_has_error.restype = ctypes.c_bool
        lib.magpie_has_error.argtypes = [ctypes.c_void_p]
        for func in (
            lib.magpie_get_and_clear_error,
            lib.magpie_get_last_command_status_message,
            lib.magpie_get_last_command_output,
        ):
            func.restype = ctypes.c_void_p
            func.argtypes = [ctypes.c_void_p]
        lib.magpie_stop_current_command.restype = None
        lib.magpie_stop_current_command.argtypes = [ctypes.c_void_p]
        lib.magpie_get_thread_status.restype = ctypes.c_int
        lib.magpie_get_thread_status.argtypes = [ctypes.c_void_p]
        lib.magpie_free_string.restype = None
        lib.magpie_free_string.argtypes = [ctypes.c_void_p]

    def _take_string(self, ptr):
        """Copies a C string returned by the library and frees the original."""
        if not ptr:
            return ""
        value = ctypes.string_at(ptr).decode("utf-8")
        self._lib.magpie_free_string(ptr)
        return value

    def _check_exit_code(self, command, exit_code):
        if exit_code == MAGPIE_SUCCESS:
            return
        error = self._take_string(self._lib.magpie_get_and_clear_error(self._mp))
        raise MagpieError(f"command '{command}' failed: {error}")

    def run(self, command, human_readable=False):
        """Runs a command to completion and returns its output."""
        if human_readable:
            run_func = self._lib.magpie_run_sync_human_readable
        else:
            run_func = self._lib.magpie_run_sync
        exit_code = run_func(self._mp, command.encode("utf-8"))
        self._check_exit_code(command, exit_code)
        return self.output()

    def run_async(self, command, human_readable=False):
        """Starts a command on a background thread."""
        if human_readable:
            run_func = self._lib.magpie_run_async_human_readable
        else:
            run_func = self._lib.magpie_run_async
        exit_code = run_func(self._mp, command.encode("utf-8"))
        self._check_exit_code(command, exit_code)

    def wait(self):
        """Waits for the running async command and returns its output."""
        exit_code = self._lib.magpie_await(self._mp)
        self._check_exit_code("<async>", exit_code)
        return self.output()

    def stop(self):
        self._lib.magpie_stop_current_command(self._mp)

    def status(self):
        """Returns the live status message of the running command."""
        return self._take_string(
            self._lib.magpie_get_last_command_status_message(self._mp)
        )

    def thread_status(self):
        return self._lib.magpie_get_thread_status(self._mp)

    def output(self):
        return self._take_string(
            self._lib.magpie_get_last_command_output(self._mp)
        )

    def close(self):
        if self._mp:
            self._lib.magpie_destroy(self._mp)
            self._mp = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.close()


def main():
    data_paths = sys.argv[1] if len(sys.argv) > 1 else "./data"
    with Magpie(data_paths) as magpie:
        print("magpie REPL; type a command, or 'exit' to quit")
        while True:
            try:
                command = input("magpie> ").strip()
            except EOFError:
                break
            if not command:
                continue
            if command in ("exit", "quit"):
                break
            try:
                print(magpie.run(command, human_readable=True), end="")
            except MagpieError as exception:
                print(exception)


if __name__ == "__main__":
    main()
