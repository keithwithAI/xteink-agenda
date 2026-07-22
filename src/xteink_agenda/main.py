"""CLI entrypoint for xteink-agenda."""

from __future__ import annotations

import argparse
import os

from dotenv import load_dotenv


def cmd_serve(args):
    """Start the FastAPI server (X4 device API)."""
    import uvicorn

    load_dotenv()

    port = args.port or int(os.getenv("PORT", "8080"))
    print(f"Starting server on http://0.0.0.0:{port}")
    print(f"  X4 API:   http://localhost:{port}/api/x4/agenda")
    print(f"  Settings: http://localhost:{port}/settings")
    print(f"  Health:   http://localhost:{port}/health")
    uvicorn.run(
        "xteink_agenda.server:app",
        host="0.0.0.0",
        port=port,
        reload=args.reload,
    )


def cli():
    parser = argparse.ArgumentParser(prog="xteink-agenda")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("serve", help="Start API server")
    p.add_argument("--port", type=int, default=None)
    p.add_argument("--reload", action="store_true", help="Auto-reload on code changes")
    p.set_defaults(func=cmd_serve)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    cli()
