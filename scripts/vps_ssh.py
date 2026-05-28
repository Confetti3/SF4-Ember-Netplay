"""Shared Paramiko SSH connect for VPS deploy scripts (host-key verification)."""

from __future__ import annotations

import os
import sys
from pathlib import Path

import paramiko


def _load_known_hosts(client: paramiko.SSHClient) -> None:
    paths: list[Path] = []
    env_path = os.environ.get("SF4E_VPS_KNOWN_HOSTS", "").strip()
    if env_path:
        paths.append(Path(env_path))
    paths.append(Path.home() / ".ssh" / "known_hosts")
    for path in paths:
        if path.is_file():
            try:
                client.load_host_keys(str(path))
                return
            except OSError:
                continue


def connect_ssh(
    host: str,
    *,
    username: str = "root",
    password: str = "",
    timeout: int = 30,
    banner_timeout: int | None = None,
    auth_timeout: int | None = None,
    allow_agent: bool = False,
    look_for_keys: bool = False,
    **kwargs: object,
) -> paramiko.SSHClient:
    """Connect with RejectPolicy unless SF4E_VPS_ACCEPT_NEW_HOST=1 (maintainer bootstrap)."""
    client = paramiko.SSHClient()
    _load_known_hosts(client)
    accept_new = os.environ.get("SF4E_VPS_ACCEPT_NEW_HOST", "").strip().lower() in (
        "1",
        "true",
        "yes",
    )
    if accept_new:
        client.set_missing_host_key_policy(paramiko.WarningPolicy())
        print(
            "WARNING: SF4E_VPS_ACCEPT_NEW_HOST — verify host key before production deploy",
            file=sys.stderr,
        )
    else:
        client.set_missing_host_key_policy(paramiko.RejectPolicy())

    connect_kw: dict = {
        "hostname": host,
        "username": username,
        "password": password,
        "timeout": timeout,
        "allow_agent": allow_agent,
        "look_for_keys": look_for_keys,
    }
    if banner_timeout is not None:
        connect_kw["banner_timeout"] = banner_timeout
    if auth_timeout is not None:
        connect_kw["auth_timeout"] = auth_timeout
    connect_kw.update(kwargs)  # type: ignore[arg-type]
    client.connect(**connect_kw)
    return client
