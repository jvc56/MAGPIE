"""Expose the current git commit hash and date to templates.

Used by overrides/main.html to stamp the header so screenshots / content dumps
can be tied back to a specific build. Configured via `hooks:` in mkdocs.yml.
"""

import subprocess


def _git(args):
    try:
        return (
            subprocess.check_output(["git", *args], stderr=subprocess.DEVNULL)
            .decode()
            .strip()
        )
    except Exception:
        return ""


def on_config(config, **kwargs):
    config.extra["build_rev"] = _git(["rev-parse", "--short", "HEAD"])
    config.extra["build_date"] = _git(["log", "-1", "--format=%cs"])
    return config
